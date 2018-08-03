/*
 * Copyright (C) 2015-2018 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gumv8platform.h"

#include "gumv8script-debug.h"
#include "gumv8script-java.h"
#include "gumv8script-objc.h"
#include "gumv8script-runtime.h"

#include <gum/gumcloak.h>
#include <gum/gummemory.h>

using namespace v8;

class GumV8MainContextOperation : public GumV8Operation
{
public:
  GumV8MainContextOperation (GumV8Platform * platform,
      std::function<void ()> func, GSource * source);
  ~GumV8MainContextOperation () override;

  void Perform ();
  void Cancel () override;
  void Await () override;

private:
  GumV8Platform * platform;
  std::function<void ()> func;
  GSource * source;
  volatile bool completed;
  GCond cond;

  friend class GumV8Platform;
};

class GumV8ThreadPoolOperation : public GumV8Operation
{
public:
  GumV8ThreadPoolOperation (GumV8Platform * platform,
      std::function<void ()> func);
  ~GumV8ThreadPoolOperation () override;

  void Perform ();
  void Cancel () override;
  void Await () override;

private:
  GumV8Platform * platform;
  std::function<void ()> func;
  volatile bool completed;
  GCond cond;
};

class GumV8ForegroundTaskRunner : public TaskRunner
{
public:
  GumV8ForegroundTaskRunner (GumV8Platform * platform, Isolate * isolate);
  ~GumV8ForegroundTaskRunner () override;

  void PostTask (std::unique_ptr<Task> task) override;
  void PostDelayedTask (std::unique_ptr<Task> task,
      double delay_in_seconds) override;
  void PostIdleTask (std::unique_ptr<IdleTask> task) override;
  bool IdleTasksEnabled () override;

private:
  void Run (Task * task);
  void Run (IdleTask * task);

  GumV8Platform * platform;
  Isolate * isolate;
  GHashTable * pending;
};

class GumV8ArrayBufferAllocator : public ArrayBuffer::Allocator
{
public:
  GumV8ArrayBufferAllocator () = default;

  void * Allocate (size_t length) override;
  void * AllocateUninitialized (size_t length) override;
  void Free (void * data, size_t length) override;
};

class GumV8MemoryBackend : public MemoryBackend
{
public:
  GumV8MemoryBackend () = default;

  void * Allocate (void * address, const size_t size, bool is_executable)
      override;
  bool Free (void * address, size_t size) override;
  bool Release (void * address, size_t size) override;

private:
  static void Cloak (gpointer base, gsize size);
  static void Uncloak (gpointer base, gsize size);
};

class GumV8ThreadingBackend : public ThreadingBackend
{
public:
  GumV8ThreadingBackend () = default;

  MutexImpl * CreatePlainMutex () override;
  MutexImpl * CreateRecursiveMutex () override;
  ConditionVariableImpl * CreateConditionVariable () override;
};

class GumMutex : public MutexImpl
{
public:
  GumMutex ();
  ~GumMutex () override;

  void Lock () override;
  void Unlock () override;
  bool TryLock () override;

private:
  GMutex mutex;

  friend class GumConditionVariable;
};

class GumRecursiveMutex : public MutexImpl
{
public:
  GumRecursiveMutex ();
  ~GumRecursiveMutex () override;

  void Lock () override;
  void Unlock () override;
  bool TryLock () override;

private:
  GRecMutex mutex;
};

class GumConditionVariable : public ConditionVariableImpl
{
public:
  GumConditionVariable ();
  ~GumConditionVariable () override;

  void NotifyOne () override;
  void NotifyAll () override;
  void Wait (MutexImpl * mutex) override;
  bool WaitFor (MutexImpl * mutex, int64_t delta_in_microseconds) override;

private:
  GCond cond;
};

class GumV8PlatformLocker
{
public:
  GumV8PlatformLocker (GumV8Platform * platform)
    : platform (platform)
  {
    g_mutex_lock (&platform->lock);
  }

  GumV8PlatformLocker (const GumV8PlatformLocker &) = delete;

  GumV8PlatformLocker & operator= (const GumV8PlatformLocker &) = delete;

  ~GumV8PlatformLocker ()
  {
    g_mutex_unlock (&platform->lock);
  }

private:
  GumV8Platform * platform;
};

class GumV8PlatformUnlocker
{
public:
  GumV8PlatformUnlocker (GumV8Platform * platform)
    : platform (platform)
  {
    g_mutex_unlock (&platform->lock);
  }

  GumV8PlatformUnlocker (const GumV8PlatformUnlocker &) = delete;

  GumV8PlatformUnlocker & operator= (const GumV8PlatformUnlocker &) = delete;

  ~GumV8PlatformUnlocker ()
  {
    g_mutex_lock (&platform->lock);
  }

private:
  GumV8Platform * platform;
};

GumV8Platform::GumV8Platform ()
  : objc_bundle (NULL),
    java_bundle (NULL),
    scheduler (gum_script_scheduler_new ()),
    start_time (g_get_monotonic_time ()),
    array_buffer_allocator (new GumV8ArrayBufferAllocator ()),
    memory_backend (new GumV8MemoryBackend ()),
    threading_backend (new GumV8ThreadingBackend ()),
    tracing_controller (new TracingController ())
{
  g_mutex_init (&lock);

  V8::InitializePlatform (this);
  V8::Initialize ();

  Isolate::CreateParams params;
  params.array_buffer_allocator = array_buffer_allocator;

  isolate = Isolate::New (params);
  isolate->SetFatalErrorHandler (OnFatalError);

  InitRuntime ();
}

GumV8Platform::~GumV8Platform ()
{
  auto dispose = ScheduleOnJSThread (G_PRIORITY_HIGH, [=]() { Dispose (); });
  dispose->Await ();

  g_object_unref (scheduler);

  delete tracing_controller;
  delete threading_backend;
  delete memory_backend;
  delete array_buffer_allocator;

  g_mutex_clear (&lock);
}

void
GumV8Platform::InitRuntime ()
{
  Locker locker (isolate);
  Isolate::Scope isolate_scope (isolate);
  HandleScope handle_scope (isolate);
  Local<Context> context (Context::New (isolate));
  Context::Scope context_scope (context);

  runtime_bundle = gum_v8_bundle_new (isolate, gumjs_runtime_modules);
  debug_bundle = gum_v8_bundle_new (isolate, gumjs_debug_modules);
}

void
GumV8Platform::Dispose ()
{
  {
    Locker locker (isolate);
    Isolate::Scope isolate_scope (isolate);
    HandleScope handle_scope (isolate);

    g_clear_pointer (&objc_bundle, gum_v8_bundle_free);
    g_clear_pointer (&java_bundle, gum_v8_bundle_free);

    g_clear_pointer (&debug_bundle, gum_v8_bundle_free);
    g_clear_pointer (&runtime_bundle, gum_v8_bundle_free);
  }

  isolate->Dispose ();

  {
    GumV8PlatformLocker locker (this);

    // FIXME: dispose runners
  }

  V8::Dispose ();
  V8::ShutdownPlatform ();
}

void
GumV8Platform::OnFatalError (const char * location,
                             const char * message)
{
  g_log ("V8", G_LOG_LEVEL_ERROR, "%s: %s", location, message);
}

const gchar *
GumV8Platform::GetRuntimeSourceMap () const
{
  return gumjs_frida_source_map;
}

GumV8Bundle *
GumV8Platform::GetObjCBundle ()
{
  if (objc_bundle == NULL)
    objc_bundle = gum_v8_bundle_new (isolate, gumjs_objc_modules);
  return objc_bundle;
}

const gchar *
GumV8Platform::GetObjCSourceMap () const
{
  return gumjs_objc_source_map;
}

GumV8Bundle *
GumV8Platform::GetJavaBundle ()
{
  if (java_bundle == NULL)
    java_bundle = gum_v8_bundle_new (isolate, gumjs_java_modules);
  return java_bundle;
}

const gchar *
GumV8Platform::GetJavaSourceMap () const
{
  return gumjs_java_source_map;
}

std::shared_ptr<GumV8Operation>
GumV8Platform::ScheduleOnJSThread (std::function<void ()> f)
{
  return ScheduleOnJSThreadDelayed (0, G_PRIORITY_DEFAULT, f);
}

std::shared_ptr<GumV8Operation>
GumV8Platform::ScheduleOnJSThread (gint priority,
                                   std::function<void ()> f)
{
  return ScheduleOnJSThreadDelayed (0, priority, f);
}

std::shared_ptr<GumV8Operation>
GumV8Platform::ScheduleOnJSThreadDelayed (guint delay_in_milliseconds,
                                          std::function<void ()> f)
{
  return ScheduleOnJSThreadDelayed (delay_in_milliseconds, G_PRIORITY_DEFAULT,
      f);
}

std::shared_ptr<GumV8Operation>
GumV8Platform::ScheduleOnJSThreadDelayed (guint delay_in_milliseconds,
                                          gint priority,
                                          std::function<void ()> f)
{
  GSource * source = (delay_in_milliseconds != 0)
      ? g_timeout_source_new (delay_in_milliseconds)
      : g_idle_source_new ();
  g_source_set_priority (source, priority);

  auto op = std::make_shared<GumV8MainContextOperation> (this, f, source);
  {
    GumV8PlatformLocker locker (this);
    js_operations.insert (op);
  }
  g_source_set_callback (source, PerformMainContextOperation,
      new std::shared_ptr<GumV8MainContextOperation> (op),
      ReleaseMainContextOperation);
  g_source_attach (source, gum_script_scheduler_get_js_context (scheduler));

  return op;
}

std::shared_ptr<GumV8Operation>
GumV8Platform::ScheduleOnThreadPool (std::function<void ()> f)
{
  auto op = std::make_shared<GumV8ThreadPoolOperation> (this, f);
  gum_script_scheduler_push_job_on_thread_pool (scheduler,
      PerformThreadPoolOperation,
      new std::shared_ptr<GumV8ThreadPoolOperation> (op),
      ReleaseThreadPoolOperation);

  return op;
}

std::shared_ptr<GumV8Operation>
GumV8Platform::ScheduleOnThreadPoolDelayed (guint delay_in_milliseconds,
                                            std::function<void ()> f)
{
  auto op = std::make_shared<GumV8ThreadPoolOperation> (this, f);
  auto child_op = ScheduleOnJSThreadDelayed (delay_in_milliseconds,
      G_PRIORITY_HIGH, [=]()
      {
        ScheduleOnThreadPool (f);
      });
}

gboolean
GumV8Platform::PerformMainContextOperation (gpointer data)
{
  auto operation = (std::shared_ptr<GumV8MainContextOperation> *) data;

  (*operation)->Perform ();

  return FALSE;
}

void
GumV8Platform::ReleaseMainContextOperation (gpointer data)
{
  auto ptr = (std::shared_ptr<GumV8MainContextOperation> *) data;

  auto op = *ptr;
  auto platform = op->platform;
  {
    GumV8PlatformLocker locker (platform);
    platform->js_operations.erase (op);
  }

  delete ptr;
}

void
GumV8Platform::PerformThreadPoolOperation (gpointer data)
{
  auto operation = (std::shared_ptr<GumV8ThreadPoolOperation> *) data;

  (*operation)->Perform ();
}

void
GumV8Platform::ReleaseThreadPoolOperation (gpointer data)
{
  auto ptr = (std::shared_ptr<GumV8ThreadPoolOperation> *) data;

  delete ptr;
}

int
GumV8Platform::NumberOfWorkerThreads ()
{
  return g_get_num_processors ();
}

std::shared_ptr<TaskRunner>
GumV8Platform::GetForegroundTaskRunner (Isolate * isolate)
{
  auto runner = foreground_runners[isolate];
  if (!runner)
  {
    runner = std::make_shared<GumV8ForegroundTaskRunner> (this, isolate);
    foreground_runners[isolate] = runner;
  }

  return runner;
}

void
GumV8Platform::CallOnWorkerThread (std::unique_ptr<Task> task)
{
  std::shared_ptr<Task> t (std::move (task));
  ScheduleOnThreadPool ([=]() { t->Run (); });
}

void
GumV8Platform::CallDelayedOnWorkerThread (std::unique_ptr<Task> task,
                                          double delay_in_seconds)
{
  std::shared_ptr<Task> t (std::move (task));
  ScheduleOnThreadPoolDelayed (delay_in_seconds * 1000.0, [=]()
      {
        t->Run ();
      });
}

void
GumV8Platform::CallOnForegroundThread (Isolate * isolate,
                                       Task * task)
{
  GetForegroundTaskRunner (isolate)->PostTask (std::unique_ptr<Task> (task));
}

void
GumV8Platform::CallDelayedOnForegroundThread (Isolate * isolate,
                                              Task * task,
                                              double delay_in_seconds)
{
  GetForegroundTaskRunner (isolate)->PostDelayedTask (
      std::unique_ptr<Task> (task), delay_in_seconds);
}

void
GumV8Platform::CallIdleOnForegroundThread (Isolate * isolate,
                                           IdleTask * task)
{
  GetForegroundTaskRunner (isolate)->PostIdleTask (
      std::unique_ptr<IdleTask> (task));
}

bool
GumV8Platform::IdleTasksEnabled (Isolate * isolate)
{
  return true;
}

double
GumV8Platform::MonotonicallyIncreasingTime ()
{
  gint64 delta = g_get_monotonic_time () - start_time;

  return ((double) (delta / G_GINT64_CONSTANT (1000))) / 1000.0;
}

double
GumV8Platform::CurrentClockTimeMillis ()
{
  return (double) (g_get_real_time () / G_GINT64_CONSTANT (1000));
}

MemoryBackend *
GumV8Platform::GetMemoryBackend ()
{
  return memory_backend;
}

ThreadingBackend *
GumV8Platform::GetThreadingBackend ()
{
  return threading_backend;
}

TracingController *
GumV8Platform::GetTracingController ()
{
  return tracing_controller;
}

GumV8MainContextOperation::GumV8MainContextOperation (
    GumV8Platform * platform,
    std::function<void ()> func,
    GSource * source)
  : platform (platform),
    func (func),
    source (source),
    completed (false)
{
  g_cond_init (&cond);
}

GumV8MainContextOperation::~GumV8MainContextOperation ()
{
  g_source_unref (source);
  g_cond_clear (&cond);
}

void
GumV8MainContextOperation::Perform ()
{
  func ();

  GumV8PlatformLocker locker (platform);
  completed = true;
  g_cond_signal (&cond);
}

void
GumV8MainContextOperation::Cancel ()
{
  g_source_destroy (source);

  GumV8PlatformLocker locker (platform);
  completed = true;
  g_cond_signal (&cond);
}

void
GumV8MainContextOperation::Await ()
{
  GumV8PlatformLocker locker (platform);
  while (!completed)
    g_cond_wait (&cond, &platform->lock);
}

GumV8ThreadPoolOperation::GumV8ThreadPoolOperation (
    GumV8Platform * platform,
    std::function<void ()> func)
  : platform (platform),
    func (func),
    completed (false)
{
  g_cond_init (&cond);
}

GumV8ThreadPoolOperation::~GumV8ThreadPoolOperation ()
{
  g_cond_clear (&cond);
}

void
GumV8ThreadPoolOperation::Perform ()
{
  func ();

  GumV8PlatformLocker locker (platform);
  completed = true;
  g_cond_signal (&cond);
}

void
GumV8ThreadPoolOperation::Cancel ()
{
}

void
GumV8ThreadPoolOperation::Await ()
{
  GumV8PlatformLocker locker (platform);
  while (!completed)
    g_cond_wait (&cond, &platform->lock);
}

GumV8ForegroundTaskRunner::GumV8ForegroundTaskRunner (GumV8Platform * platform,
                                                      Isolate * isolate)
  : platform (platform),
    isolate (isolate),
    pending (g_hash_table_new (NULL, NULL))
{
}

GumV8ForegroundTaskRunner::~GumV8ForegroundTaskRunner ()
{
  g_hash_table_unref (pending);
}

void
GumV8ForegroundTaskRunner::PostTask (std::unique_ptr<Task> task)
{
  std::shared_ptr<Task> t (std::move (task));
  platform->ScheduleOnJSThread ([=]()
      {
        Run (t.get ());
      });
}

void
GumV8ForegroundTaskRunner::PostDelayedTask (std::unique_ptr<Task> task,
                                            double delay_in_seconds)
{
  std::shared_ptr<Task> t (std::move (task));
  platform->ScheduleOnJSThreadDelayed (delay_in_seconds * 1000.0, [=]()
      {
        Run (t.get ());
      });
}

void
GumV8ForegroundTaskRunner::PostIdleTask (std::unique_ptr<IdleTask> task)
{
  std::shared_ptr<IdleTask> t (std::move (task));
  platform->ScheduleOnJSThread (G_PRIORITY_LOW, [=]()
      {
        Run (t.get ());
      });
}

bool
GumV8ForegroundTaskRunner::IdleTasksEnabled ()
{
  return true;
}

void
GumV8ForegroundTaskRunner::Run (Task * task)
{
  Locker locker (isolate);
  Isolate::Scope isolate_scope (isolate);
  HandleScope handle_scope (isolate);

  task->Run ();
}

void
GumV8ForegroundTaskRunner::Run (IdleTask * task)
{
  Locker locker (isolate);
  Isolate::Scope isolate_scope (isolate);
  HandleScope handle_scope (isolate);

  const double deadline_in_seconds =
      platform->MonotonicallyIncreasingTime () + (1.0 / 60.0);
  task->Run (deadline_in_seconds);
}

void *
GumV8ArrayBufferAllocator::Allocate (size_t length)
{
  return g_malloc0 (length);
}

void *
GumV8ArrayBufferAllocator::AllocateUninitialized (size_t length)
{
  return g_malloc (length);
}

void
GumV8ArrayBufferAllocator::Free (void * data,
                                 size_t length)
{
  g_free (data);
}

void *
GumV8MemoryBackend::Allocate (void * address,
                              const size_t size,
                              bool is_executable)
{
  gpointer base = gum_memory_allocate (size,
      is_executable ? GUM_PAGE_RWX : GUM_PAGE_RW, address);
  if (base != NULL)
    Cloak (base, size);
  return base;
}

bool
GumV8MemoryBackend::Free (void * address,
                          size_t size)
{
  bool success = !!gum_memory_release (address, size);
  if (success)
    Uncloak (address, size);
  return success;
}

bool
GumV8MemoryBackend::Release (void * address,
                             size_t size)
{
  // FIXME
  return Free (address, size);
}

void
GumV8MemoryBackend::Cloak (gpointer base,
                           gsize size)
{
  GumMemoryRange r;
  r.base_address = GUM_ADDRESS (base);
  r.size = size;
  gum_cloak_add_range (&r);
}

void
GumV8MemoryBackend::Uncloak (gpointer base,
                             gsize size)
{
  GumMemoryRange r;
  r.base_address = GUM_ADDRESS (base);
  r.size = size;
  gum_cloak_remove_range (&r);
}

MutexImpl *
GumV8ThreadingBackend::CreatePlainMutex ()
{
  return new GumMutex ();
}

MutexImpl *
GumV8ThreadingBackend::CreateRecursiveMutex ()
{
  return new GumRecursiveMutex ();
}

ConditionVariableImpl *
GumV8ThreadingBackend::CreateConditionVariable ()
{
  return new GumConditionVariable ();
}

GumMutex::GumMutex ()
{
  g_mutex_init (&mutex);
}

GumMutex::~GumMutex ()
{
  g_mutex_clear (&mutex);
}

void
GumMutex::Lock ()
{
  g_mutex_lock (&mutex);
}

void
GumMutex::Unlock ()
{
  g_mutex_unlock (&mutex);
}

bool
GumMutex::TryLock ()
{
  return !!g_mutex_trylock (&mutex);
}

GumRecursiveMutex::GumRecursiveMutex ()
{
  g_rec_mutex_init (&mutex);
}

GumRecursiveMutex::~GumRecursiveMutex ()
{
  g_rec_mutex_clear (&mutex);
}

void
GumRecursiveMutex::Lock ()
{
  g_rec_mutex_lock (&mutex);
}

void
GumRecursiveMutex::Unlock ()
{
  g_rec_mutex_unlock (&mutex);
}

bool
GumRecursiveMutex::TryLock ()
{
  return !!g_rec_mutex_trylock (&mutex);
}

GumConditionVariable::GumConditionVariable ()
{
  g_cond_init (&cond);
}

GumConditionVariable::~GumConditionVariable ()
{
  g_cond_clear (&cond);
}

void
GumConditionVariable::NotifyOne ()
{
  g_cond_signal (&cond);
}

void
GumConditionVariable::NotifyAll ()
{
  g_cond_broadcast (&cond);
}

void
GumConditionVariable::Wait (MutexImpl * mutex)
{
  GumMutex * m = (GumMutex *) mutex;
  g_cond_wait (&cond, &m->mutex);
}

bool
GumConditionVariable::WaitFor (MutexImpl * mutex,
                               int64_t delta_in_microseconds)
{
  GumMutex * m = (GumMutex *) mutex;
  gint64 deadline = g_get_monotonic_time () + delta_in_microseconds;
  return !!g_cond_wait_until (&cond, &m->mutex, deadline);
}
