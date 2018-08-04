/*
 * Copyright (C) 2010-2017 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 * Copyright (C) 2013 Karl Trygve Kalleberg <karltk@boblycat.org>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "testutil.h"

#include "gum-init.h"
#include "gumdukscriptbackend.h"
#include "gumscriptbackend.h"
#include "valgrind.h"

#include <stdio.h>
#include <string.h>
#include <gio/gio.h>
#ifdef G_OS_WIN32
#ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
#endif
# include <tchar.h>
# include <windows.h>
# include <winsock2.h>
# include <ws2tcpip.h>
#else
# include <errno.h>
# include <fcntl.h>
# include <netinet/in.h>
# include <sys/socket.h>
# include <sys/un.h>
# include <unistd.h>
#endif
#ifdef HAVE_QNX
# include <unix.h>
#endif

#define ANY_LINE_NUMBER -1
#define SCRIPT_MESSAGE_DEFAULT_TIMEOUT_MSEC 500

#ifndef SCRIPT_SUITE
# define SCRIPT_SUITE ""
#endif
#define SCRIPT_TESTCASE(NAME) \
    void test_script_ ## NAME (TestScriptFixture * fixture, gconstpointer data)
#define SCRIPT_TESTENTRY(NAME)                                            \
  G_STMT_START                                                            \
  {                                                                       \
    extern void test_script_ ##NAME (TestScriptFixture * fixture,         \
        gconstpointer data);                                              \
    gchar * path;                                                         \
                                                                          \
    path = g_strconcat ("/GumJS/Script/" SCRIPT_SUITE "/" #NAME "#",      \
        GUM_DUK_IS_SCRIPT_BACKEND (fixture_data) ? "DUK" : "V8",          \
        NULL);                                                            \
                                                                          \
    g_test_add (path,                                                     \
        TestScriptFixture,                                                \
        fixture_data,                                                     \
        test_script_fixture_setup,                                        \
        test_script_ ##NAME,                                              \
        test_script_fixture_teardown);                                    \
                                                                          \
    g_free (path);                                                        \
  }                                                                       \
  G_STMT_END;

#define COMPILE_AND_LOAD_SCRIPT(SOURCE, ...) \
    test_script_fixture_compile_and_load_script (fixture, SOURCE, \
    ## __VA_ARGS__)
#define UNLOAD_SCRIPT() \
    gum_script_unload_sync (fixture->script, NULL); \
    g_object_unref (fixture->script); \
    fixture->script = NULL;
#define POST_MESSAGE(MSG) \
    gum_script_post (fixture->script, MSG, NULL)
#define EXPECT_NO_MESSAGES() \
    g_assert (test_script_fixture_try_pop_message (fixture, 1) == NULL)
#define EXPECT_SEND_MESSAGE_WITH(PAYLOAD, ...) \
    test_script_fixture_expect_send_message_with (fixture, PAYLOAD, \
    ## __VA_ARGS__)
#define EXPECT_SEND_MESSAGE_WITH_PREFIX(PREFIX, ...) \
    test_script_fixture_expect_send_message_with_prefix (fixture, PREFIX, \
    ## __VA_ARGS__)
#define EXPECT_SEND_MESSAGE_WITH_PAYLOAD_AND_DATA(PAYLOAD, DATA) \
    test_script_fixture_expect_send_message_with_payload_and_data (fixture, \
        PAYLOAD, DATA)
#define EXPECT_ERROR_MESSAGE_WITH(LINE_NUMBER, DESC) \
    test_script_fixture_expect_error_message_with (fixture, LINE_NUMBER, DESC)
#define EXPECT_LOG_MESSAGE_WITH(LEVEL, PAYLOAD, ...) \
    test_script_fixture_expect_log_message_with (fixture, LEVEL, PAYLOAD, \
    ## __VA_ARGS__)
#define PUSH_TIMEOUT(value) test_script_fixture_push_timeout (fixture, value)
#define POP_TIMEOUT() test_script_fixture_pop_timeout (fixture)

#define GUM_PTR_CONST "ptr(\"0x%" G_GSIZE_MODIFIER "x\")"

#ifdef G_OS_WIN32
# define GUM_CLOSE_SOCKET(s) closesocket (s)
#else
# define GUM_CLOSE_SOCKET(s) close (s)
#endif

#if defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 4
# define GUM_RETURN_VALUE_REGISTER_NAME "eax"
#elif defined (HAVE_I386) && GLIB_SIZEOF_VOID_P == 8
# define GUM_RETURN_VALUE_REGISTER_NAME "rax"
#elif defined (HAVE_ARM)
# define GUM_RETURN_VALUE_REGISTER_NAME "r0"
#elif defined (HAVE_ARM64)
# define GUM_RETURN_VALUE_REGISTER_NAME "x0"
#elif defined (HAVE_MIPS)
# define GUM_RETURN_VALUE_REGISTER_NAME "v0"
#else
# error Unsupported architecture
#endif

typedef struct _TestScriptFixture
{
  GumScriptBackend * backend;
  GumScript * script;
  GMainLoop * loop;
  GMainContext * context;
  GQueue messages;
  GQueue timeouts;
} TestScriptFixture;

typedef struct _TestScriptMessageItem
{
  gchar * message;
  gchar * data;
} TestScriptMessageItem;

static void test_script_message_item_free (TestScriptMessageItem * item);
static TestScriptMessageItem * test_script_fixture_try_pop_message (
    TestScriptFixture * fixture, guint timeout);
static gboolean test_script_fixture_stop_loop (TestScriptFixture * fixture);
static void test_script_fixture_expect_send_message_with_prefix (
    TestScriptFixture * fixture, const gchar * prefix_template, ...);
static void test_script_fixture_expect_send_message_with_payload_and_data (
    TestScriptFixture * fixture, const gchar * payload, const gchar * data);
static void test_script_fixture_expect_error_message_with (
    TestScriptFixture * fixture, gint line_number, const gchar * description);
static void test_script_fixture_expect_log_message_with (
    TestScriptFixture * fixture, const gchar * level,
    const gchar * payload_template, ...);
static void test_script_fixture_push_timeout (TestScriptFixture * fixture,
    guint timeout);
static void test_script_fixture_pop_timeout (TestScriptFixture * fixture);

static GumExceptor * exceptor = NULL;

static void
test_script_fixture_deinit (void)
{
  g_object_unref (exceptor);
  exceptor = NULL;
}

static void
test_script_fixture_setup (TestScriptFixture * fixture,
                           gconstpointer data)
{
  (void) test_script_fixture_expect_send_message_with_prefix;
  (void) test_script_fixture_expect_send_message_with_payload_and_data;
  (void) test_script_fixture_expect_error_message_with;
  (void) test_script_fixture_expect_log_message_with;
  (void) test_script_fixture_pop_timeout;

  fixture->backend = (GumScriptBackend *) data;
  fixture->context = g_main_context_ref_thread_default ();
  fixture->loop = g_main_loop_new (fixture->context, FALSE);
  g_queue_init (&fixture->messages);
  g_queue_init (&fixture->timeouts);

  test_script_fixture_push_timeout (fixture,
      SCRIPT_MESSAGE_DEFAULT_TIMEOUT_MSEC);

  if (exceptor == NULL)
  {
    exceptor = gum_exceptor_obtain ();
    _gum_register_destructor (test_script_fixture_deinit);
  }
}

static void
test_script_fixture_teardown (TestScriptFixture * fixture,
                              gconstpointer data)
{
  TestScriptMessageItem * item;

  if (fixture->script != NULL)
  {
    gum_script_unload_sync (fixture->script, NULL);
    g_object_unref (fixture->script);
  }

  while (g_main_context_pending (fixture->context))
    g_main_context_iteration (fixture->context, FALSE);

  while ((item = test_script_fixture_try_pop_message (fixture, 1)) != NULL)
  {
    test_script_message_item_free (item);
  }

  g_queue_clear (&fixture->timeouts);

  g_main_loop_unref (fixture->loop);
  g_main_context_unref (fixture->context);
}

static void
test_script_message_item_free (TestScriptMessageItem * item)
{
  g_free (item->message);
  g_free (item->data);
  g_slice_free (TestScriptMessageItem, item);
}

static void
test_script_fixture_store_message (GumScript * script,
                                   const gchar * message,
                                   GBytes * data,
                                   gpointer user_data)
{
  TestScriptFixture * self = (TestScriptFixture *) user_data;
  TestScriptMessageItem * item;

  item = g_slice_new (TestScriptMessageItem);
  item->message = g_strdup (message);

  if (data != NULL)
  {
    const guint8 * data_elements;
    gsize data_size, i;
    GString * s;

    data_elements = g_bytes_get_data (data, &data_size);

    s = g_string_sized_new (3 * data_size);
    for (i = 0; i != data_size; i++)
    {
      if (i != 0)
        g_string_append_c (s, ' ');
      g_string_append_printf (s, "%02x", (int) data_elements[i]);
    }

    item->data = g_string_free (s, FALSE);
  }
  else
  {
    item->data = NULL;
  }

  g_queue_push_tail (&self->messages, item);
  g_main_loop_quit (self->loop);
}

static void
test_script_fixture_compile_and_load_script (TestScriptFixture * fixture,
                                             const gchar * source_template,
                                             ...)
{
  va_list args;
  gchar * raw_source, * source;
  GError * err = NULL;

  if (fixture->script != NULL)
  {
    gum_script_unload_sync (fixture->script, NULL);
    g_object_unref (fixture->script);
    fixture->script = NULL;
  }

  va_start (args, source_template);
  raw_source = g_strdup_vprintf (source_template, args);
  va_end (args);

  source = g_strconcat ("\"use strict\"; ", raw_source, NULL);

  fixture->script = gum_script_backend_create_sync (fixture->backend,
      "testcase", source, NULL, &err);
  if (err != NULL)
    g_printerr ("%s\n", err->message);
  g_assert (fixture->script != NULL);
  g_assert (err == NULL);

  g_free (source);
  g_free (raw_source);

  gum_script_set_message_handler (fixture->script,
      test_script_fixture_store_message, fixture, NULL);

  gum_script_load_sync (fixture->script, NULL);
}

static TestScriptMessageItem *
test_script_fixture_try_pop_message (TestScriptFixture * fixture,
                                     guint timeout)
{
  if (g_queue_is_empty (&fixture->messages))
  {
    GSource * source;

    source = g_timeout_source_new (timeout);
    g_source_set_callback (source, (GSourceFunc) test_script_fixture_stop_loop,
        fixture, NULL);
    g_source_attach (source, fixture->context);

    g_main_loop_run (fixture->loop);

    g_source_destroy (source);
    g_source_unref (source);
  }

  return g_queue_pop_head (&fixture->messages);
}

static gboolean
test_script_fixture_stop_loop (TestScriptFixture * fixture)
{
  g_main_loop_quit (fixture->loop);

  return FALSE;
}

static TestScriptMessageItem *
test_script_fixture_pop_message (TestScriptFixture * fixture)
{
  guint timeout;
  TestScriptMessageItem * item;

  timeout = GPOINTER_TO_UINT (g_queue_peek_tail (&fixture->timeouts));

  item = test_script_fixture_try_pop_message (fixture, timeout);
  g_assert (item != NULL);

  return item;
}

static void
test_script_fixture_expect_send_message_with (TestScriptFixture * fixture,
                                              const gchar * payload_template,
                                              ...)
{
//   va_list args;
//   gchar * payload;
//   TestScriptMessageItem * item;
//   gchar * expected_message;
// 
//   va_start (args, payload_template);
//   payload = g_strdup_vprintf (payload_template, args);
//   va_end (args);
// 
//   item = test_script_fixture_pop_message (fixture);
//   expected_message =
//       g_strconcat ("{\"type\":\"send\",\"payload\":", payload, "}", NULL);
//   g_assert_cmpstr (item->message, ==, expected_message);
//   test_script_message_item_free (item);
//   g_free (expected_message);
// 
//   g_free (payload);
}

static void
test_script_fixture_expect_send_message_with_prefix (
    TestScriptFixture * fixture,
    const gchar * prefix_template,
    ...)
{
//   va_list args;
//   gchar * prefix;
//   TestScriptMessageItem * item;
//   gchar * expected_message_prefix;
// 
//   va_start (args, prefix_template);
//   prefix = g_strdup_vprintf (prefix_template, args);
//   va_end (args);
// 
//   item = test_script_fixture_pop_message (fixture);
//   expected_message_prefix =
//       g_strconcat ("{\"type\":\"send\",\"payload\":", prefix, NULL);
//   g_assert (g_str_has_prefix (item->message, expected_message_prefix));
//   test_script_message_item_free (item);
//   g_free (expected_message_prefix);
// 
//   g_free (prefix);
}

static void
test_script_fixture_expect_send_message_with_payload_and_data (
    TestScriptFixture * fixture,
    const gchar * payload,
    const gchar * data)
{
//  TestScriptMessageItem * item;
//  gchar * expected_message;
//
//  item = test_script_fixture_pop_message (fixture);
//  expected_message =
//      g_strconcat ("{\"type\":\"send\",\"payload\":", payload, "}", NULL);
//  g_assert_cmpstr (item->message, ==, expected_message);
//  if (data != NULL)
//  {
//    g_assert (item->data != NULL);
//    g_assert_cmpstr (item->data, ==, data);
//  }
//  else
//  {
//    g_assert (item->data == NULL);
//  }
//  test_script_message_item_free (item);
//  g_free (expected_message);
}

static void
test_script_fixture_expect_error_message_with (TestScriptFixture * fixture,
                                               gint line_number,
                                               const gchar * description)
{
//  TestScriptMessageItem * item;
//  gchar actual_description[1024];
//  gchar actual_stack[1024];
//  gchar actual_file_name[64];
//  gint actual_line_number;
//  gint actual_column_number;
//
//  item = test_script_fixture_pop_message (fixture);
//
//  actual_description[0] = '\0';
//  actual_stack[0] = '\0';
//  actual_file_name[0] = '\0';
//  actual_line_number = -1;
//  actual_column_number = -1;
//  sscanf (item->message, "{"
//          "\"type\":\"error\","
//          "\"description\":\"%[^\"]\","
//          "\"stack\":\"%[^\"]\","
//          "\"fileName\":\"%[^\"]\","
//          "\"lineNumber\":%d,"
//          "\"columnNumber\":%d"
//      "}",
//      actual_description,
//      actual_stack,
//      actual_file_name,
//      &actual_line_number,
//      &actual_column_number);
//  if (actual_column_number == -1)
//  {
//    sscanf (item->message, "{"
//            "\"type\":\"error\","
//            "\"description\":\"%[^\"]\""
//        "}",
//        actual_description);
//  }
//  if (line_number != ANY_LINE_NUMBER)
//    g_assert_cmpint (actual_line_number, ==, line_number);
//  g_assert_cmpstr (actual_description, ==, description);
//  test_script_message_item_free (item);
}

static void
test_script_fixture_expect_log_message_with (TestScriptFixture * fixture,
                                             const gchar * level,
                                             const gchar * payload_template,
                                             ...)
{
  va_list args;
  gchar * payload;
  TestScriptMessageItem * item;
  gchar * expected_message;

  va_start (args, payload_template);
  payload = g_strdup_vprintf (payload_template, args);
  va_end (args);

  item = test_script_fixture_pop_message (fixture);
  expected_message = g_strconcat ("{\"type\":\"log\",\"level\":\"", level,
      "\",\"payload\":\"", payload, "\"}", NULL);
  g_assert_cmpstr (item->message, ==, expected_message);
  test_script_message_item_free (item);
  g_free (expected_message);

  g_free (payload);
}

static void
test_script_fixture_push_timeout (TestScriptFixture * fixture,
                                  guint timeout)
{
  g_queue_push_tail (&fixture->timeouts, GUINT_TO_POINTER (timeout));
}

static void
test_script_fixture_pop_timeout (TestScriptFixture * fixture)
{
  g_queue_pop_tail (&fixture->timeouts);
}
