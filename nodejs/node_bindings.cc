// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "xwalk/nodejs/node_bindings.h"

#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/base_paths.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/message_loop/message_loop.h"
#include "base/path_service.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/common/content_paths.h"
//FIXME #include "third_party/WebKit/public/web/WebScopedMicrotaskSuppression.h"
#include "xwalk/nodejs/node_includes.h"

using content::BrowserThread;

// The "v8::Function::kLineOffsetNotFound" is exported in node.dll, but the
// linker can not find it, could be a bug of VS.
#if defined(OS_WIN) && !defined(DEBUG)
namespace v8 {
const int Function::kLineOffsetNotFound = -1;
}
#endif

namespace xwalk {

namespace nodejs {

namespace {

// Empty callback for async handle.
void UvNoOp(uv_async_t* handle) {
}

// Convert the given vector to an array of C-strings. The strings in the
// returned vector are only guaranteed valid so long as the vector of strings
// is not modified.
std::unique_ptr<const char*[]> StringVectorToArgArray(
    const std::vector<std::string>& vector) {
  std::unique_ptr<const char*[]> array(new const char*[vector.size()]);
  for (size_t i = 0; i < vector.size(); ++i) {
    array[i] = vector[i].c_str();
  }
  return array;
}

base::FilePath GetResourcesPath(bool is_browser) {
  auto command_line = base::CommandLine::ForCurrentProcess();
  base::FilePath exec_path(command_line->GetProgram());
  PathService::Get(base::FILE_EXE, &exec_path);

  base::FilePath resources_path =
#if defined(OS_MACOSX)
      is_browser ? exec_path.DirName().DirName().Append("Resources") :
                   exec_path.DirName().DirName().DirName().DirName().DirName()
                            .Append("Resources");
#else
      exec_path.DirName().Append(FILE_PATH_LITERAL("resources"));
#endif
  return resources_path;
}

}  // namespace

NodeBindings::NodeBindings(bool is_browser)
    : is_browser_(is_browser),
      message_loop_(nullptr),
      uv_loop_(uv_default_loop()),
      embed_closed_(false),
      uv_env_(nullptr),
      weak_factory_(this) {
}

NodeBindings::~NodeBindings() {
  // Quit the embed thread.
  embed_closed_ = true;
  uv_sem_post(&embed_sem_);
  WakeupEmbedThread();

  // Wait for everything to be done.
  uv_thread_join(&embed_thread_);

  // Clear uv.
  uv_sem_destroy(&embed_sem_);
}

void NodeBindings::DidCreateScriptContext(
    blink::WebLocalFrame* frame, v8::Handle<v8::Context> context) {
  // Whether the node binding has been initialized.
  bool first_time = uv_env() == nullptr;

  // Prepare the node bindings.
  if (first_time) {
    Initialize();
    PrepareMessageLoop();
  }

  //FIXME blink::WebScopedMicrotaskSuppression suppression;
  // Setup node environment for each window.
  node::Environment* env = CreateEnvironment(context);

  // Load everything.
  LoadEnvironment(env);

  if (first_time) {
    // Make uv loop being wrapped by window context.
    set_uv_env(env);

    // Give the node loop a run to make sure everything is ready.
    RunMessageLoop();
  }
}

void NodeBindings::WillReleaseScriptContext(
    blink::WebLocalFrame* frame, v8::Handle<v8::Context> context) {
  //node::Environment* env = node::Environment::GetCurrent(context);
  //mate::EmitEvent(env->isolate(), env->process_object(), "exit");
}

void NodeBindings::Initialize() {
  // Open node's error reporting system for browser process.
  node::g_standalone_mode = is_browser_;
  node::g_upstream_node_mode = false;

  // Init node.
  // (we assume node::Init would not modify the parameters under embedded mode).
  node::Init(nullptr, nullptr, nullptr, nullptr);

#if defined(OS_WIN)
  // uv_init overrides error mode to suppress the default crash dialog, bring
  // it back if user wants to show it.
  std::unique_ptr<base::Environment> env(base::Environment::Create());
  if (env->HasVar("ELECTRON_DEFAULT_ERROR_MODE"))
    SetErrorMode(0);
#endif
}

node::Environment* NodeBindings::CreateEnvironment(
    v8::Handle<v8::Context> context) {
  std::vector<std::string> args = base::CommandLine::ForCurrentProcess()->argv();

  // Feed node the path to initialization script.
  base::FilePath::StringType process_type = FILE_PATH_LITERAL("renderer");
  base::FilePath resources_path = GetResourcesPath(false);
  base::FilePath script_path =
      resources_path.Append(process_type)
                    .Append(FILE_PATH_LITERAL("init.js"));
  LOG(INFO) <<__FUNCTION__<<", LINE "<<__LINE__
      <<", resources_path="<<resources_path.AsUTF8Unsafe().c_str()
      <<", script_path="<<script_path.AsUTF8Unsafe().c_str();
  //std::string script_path_str = script_path.AsUTF8Unsafe();
  std::string script_path_str = "/data/local/tmp/node/resources/init.js";
  args.insert(args.begin() + 1, script_path_str.c_str());

  std::unique_ptr<const char*[]> c_argv = StringVectorToArgArray(args);
  node::Environment* env = node::CreateEnvironment(
      context->GetIsolate(), uv_default_loop(), context,
      args.size(), c_argv.get(), 0, nullptr);

  return env;
}

void NodeBindings::LoadEnvironment(node::Environment* env) {
  node::LoadEnvironment(env);
  //mate::EmitEvent(env->isolate(), env->process_object(), "loaded");
}

void NodeBindings::PrepareMessageLoop() {
  DCHECK(!is_browser_ || BrowserThread::CurrentlyOn(BrowserThread::UI));

  // Add dummy handle for libuv, otherwise libuv would quit when there is
  // nothing to do.
  uv_async_init(uv_loop_, &dummy_uv_handle_, UvNoOp);

  // Start worker that will interrupt main loop when having uv events.
  uv_sem_init(&embed_sem_, 0);
  uv_thread_create(&embed_thread_, EmbedThreadRunner, this);
}

void NodeBindings::RunMessageLoop() {
  DCHECK(!is_browser_ || BrowserThread::CurrentlyOn(BrowserThread::UI));

  // The MessageLoop should have been created, remember the one in main thread.
  message_loop_ = base::MessageLoop::current();

  // Run uv loop for once to give the uv__io_poll a chance to add all events.
  UvRunOnce();
}

void NodeBindings::UvRunOnce() {
  DCHECK(!is_browser_ || BrowserThread::CurrentlyOn(BrowserThread::UI));

  node::Environment* env = uv_env();
  CHECK(env);

  v8::HandleScope handle_scope(env->isolate());

  // Enter node context while dealing with uv events.
  v8::Context::Scope context_scope(env->context());

  /*
  // Perform microtask checkpoint after running JavaScript.
  std::unique_ptr<v8::MicrotasksScope> script_scope(is_browser_ ?
      nullptr :
      new v8::MicrotasksScope(env->isolate(),
                              v8::MicrotasksScope::kRunMicrotasks));
  */
  //FIXME blink::WebScopedMicrotaskSuppression suppression;

  // Deal with uv events.
  int r = uv_run(uv_loop_, UV_RUN_NOWAIT);
  if (r == 0 || uv_loop_->stop_flag != 0)
    message_loop_->QuitWhenIdle();  // Quit from uv.

  // Tell the worker thread to continue polling.
  uv_sem_post(&embed_sem_);
}

void NodeBindings::WakeupMainThread() {
  DCHECK(message_loop_);
  message_loop_->PostTask(FROM_HERE, base::Bind(&NodeBindings::UvRunOnce,
                                                weak_factory_.GetWeakPtr()));
}

void NodeBindings::WakeupEmbedThread() {
  uv_async_send(&dummy_uv_handle_);
}

// static
void NodeBindings::EmbedThreadRunner(void *arg) {
  NodeBindings* self = static_cast<NodeBindings*>(arg);

  while (true) {
    // Wait for the main loop to deal with events.
    uv_sem_wait(&self->embed_sem_);
    if (self->embed_closed_)
      break;

    // Wait for something to happen in uv loop.
    // Note that the PollEvents() is implemented by derived classes, so when
    // this class is being destructed the PollEvents() would not be available
    // anymore. Because of it we must make sure we only invoke PollEvents()
    // when this class is alive.
    self->PollEvents();
    if (self->embed_closed_)
      break;

    // Deal with event in main thread.
    self->WakeupMainThread();
  }
}

}  // nodejs

}  // namespace xwalk
