
/*
 * Compile with: g++ -Wall fuse-bindings.cc `pkg-config fuse3 --cflags --libs` -o fuse-bindings
 */

#define FUSE_USE_VERSION 31

//#include <config.h>

#include <nan.h>
#include <fuse_lowlevel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <unordered_map>

#include <iostream>
#include <fstream>
#include <thread>

using namespace v8;

#define LOCAL_STRING(s) Nan::New<String>(s).ToLocalChecked()
#define LOCAL_NUMBER(i) Nan::New<Number>(i)
#define LOOKUP_CALLBACK(map, name) map->Has(LOCAL_STRING(name)) ? new Nan::Callback(map->Get(LOCAL_STRING(name)).As<Function>()) : NULL

enum operations_constants {
  OP_INIT = 0,
  OP_ERROR,
  OP_ACCESS,
  OP_STATFS,
  OP_FGETATTR,
  OP_GETATTR,
  OP_FLUSH,
  OP_FSYNC,
  OP_FSYNCDIR,
  OP_READDIR,
  OP_TRUNCATE,
  OP_FTRUNCATE,
  OP_UTIMENS,
  OP_READLINK,
  OP_CHOWN,
  OP_CHMOD,
  OP_MKNOD,
  OP_SETXATTR,
  OP_GETXATTR,
  OP_LISTXATTR,
  OP_REMOVEXATTR,
  OP_OPEN,
  OP_OPENDIR,
  OP_READ,
  OP_WRITE,
  OP_RELEASE,
  OP_RELEASEDIR,
  OP_CREATE,
  OP_UNLINK,
  OP_RENAME,
  OP_LINK,
  OP_SYMLINK,
  OP_MKDIR,
  OP_RMDIR,
  OP_DESTROY
};

struct bindings_template {
  Nan::Callback *getattr;
  Nan::Callback *stat;
  Nan::Callback *readdir;
  Nan::Callback *lookup;
};

struct operation_template {
  uint32_t index;
  int type;
  fuse_req_t *req;
  fuse_ino_t ino;
};

/*ToRemove*/ 
static const char *hello_str = "Hello World!\n";
static const char *hello_name = "hello";

static uv_loop_t *loop;
static uv_async_t loop_async;
static struct bindings_template bindings;
static Nan::Callback *callback_constructor;
static std::unordered_map<std::uint32_t, operation_template *> operations_map = {};
static uint32_t operation_count = 0;

NAN_INLINE static void bindings_set_date (struct timespec *out, Local<Date> date) {
  double ms = date->NumberValue();
  time_t secs = (time_t)(ms / 1000.0);
  time_t rem = ms - (1000.0 * secs);
  time_t ns = rem * 1000000.0;
  out->tv_sec = secs;
  out->tv_nsec = ns;
}

NAN_INLINE static void bindings_set_stat (struct stat *stat, Local<Object> obj) {
  if (obj->Has(LOCAL_STRING("dev"))) stat->st_dev = obj->Get(LOCAL_STRING("dev"))->NumberValue();
  if (obj->Has(LOCAL_STRING("ino"))) stat->st_ino = obj->Get(LOCAL_STRING("ino"))->NumberValue();
  if (obj->Has(LOCAL_STRING("mode"))) stat->st_mode = obj->Get(LOCAL_STRING("mode"))->Uint32Value();
  if (obj->Has(LOCAL_STRING("nlink"))) stat->st_nlink = obj->Get(LOCAL_STRING("nlink"))->NumberValue();
  if (obj->Has(LOCAL_STRING("uid"))) stat->st_uid = obj->Get(LOCAL_STRING("uid"))->NumberValue();
  if (obj->Has(LOCAL_STRING("gid"))) stat->st_gid = obj->Get(LOCAL_STRING("gid"))->NumberValue();
  if (obj->Has(LOCAL_STRING("rdev"))) stat->st_rdev = obj->Get(LOCAL_STRING("rdev"))->NumberValue();
  if (obj->Has(LOCAL_STRING("size"))) stat->st_size = obj->Get(LOCAL_STRING("size"))->NumberValue();
  if (obj->Has(LOCAL_STRING("blocks"))) stat->st_blocks = obj->Get(LOCAL_STRING("blocks"))->NumberValue();
  if (obj->Has(LOCAL_STRING("blksize"))) stat->st_blksize = obj->Get(LOCAL_STRING("blksize"))->NumberValue();
#ifdef __APPLE__
  if (obj->Has(LOCAL_STRING("mtime"))) bindings_set_date(&stat->st_mtimespec, obj->Get(LOCAL_STRING("mtime")).As<Date>());
  if (obj->Has(LOCAL_STRING("ctime"))) bindings_set_date(&stat->st_ctimespec, obj->Get(LOCAL_STRING("ctime")).As<Date>());
  if (obj->Has(LOCAL_STRING("atime"))) bindings_set_date(&stat->st_atimespec, obj->Get(LOCAL_STRING("atime")).As<Date>());
#else
  if (obj->Has(LOCAL_STRING("mtime"))) bindings_set_date(&stat->st_mtim, obj->Get(LOCAL_STRING("mtime")).As<Date>());
  if (obj->Has(LOCAL_STRING("ctime"))) bindings_set_date(&stat->st_ctim, obj->Get(LOCAL_STRING("ctime")).As<Date>());
  if (obj->Has(LOCAL_STRING("atime"))) bindings_set_date(&stat->st_atim, obj->Get(LOCAL_STRING("atime")).As<Date>());
#endif
}

NAN_METHOD(OpCallback){

  operation_template *operation = operations_map[ info[ 0 ]->Uint32Value() ];
  operations_map.erase( operation->index );
  int result = (info.Length() > 1 && info[1]->IsNumber()) ? info[1]->Uint32Value() : 0;

  if( operation->type == OP_GETATTR ){
    
    if(result == 0 && info.Length() > 2 && info[2]->IsObject()){

      struct stat stbuf;
      memset( &stbuf, 0, sizeof(stbuf) );
      bindings_set_stat( &stbuf, info[2].As<Object>());
      fuse_reply_attr(*(operation->req), &stbuf, 1.0);

    }else{
      fuse_reply_err(*(operation->req), ENOENT);
    }

  }else{
    fprintf(stderr, "OpCallback => %s\n", "not implemented");
  }

}

static void uv_handler(uv_async_t *handle){
  Nan::HandleScope scope;

  operation_template *operation = (operation_template *) handle->data;
  Local<Value> tmp[] = { Nan::New<Number>(operation->index), Nan::New<FunctionTemplate>(OpCallback)->GetFunction() };
  Nan::Callback *callbackGenerator = new Nan::Callback(callback_constructor->Call(2, tmp).As<Function>());
  Local<Function> callback = callbackGenerator->GetFunction();

  if( operation->type == OP_GETATTR ){
    
    Local<Value> tmp[] = {LOCAL_NUMBER(operation->ino), callback};
    bindings.getattr->Call( 2, tmp );

  }else{
    fprintf(stderr, "uv_handler => %s\n", "not implemented");
  }

}

static int hello_stat(fuse_ino_t ino, struct stat *stbuf){
  fprintf(stderr, "%s\n", "stat");
  stbuf->st_ino = ino;
  switch (ino) {
  case 1:
    stbuf->st_mode = S_IFDIR | 0755;
    stbuf->st_nlink = 2;
    break;

  case 2:
    stbuf->st_mode = S_IFREG | 0444;
    stbuf->st_nlink = 1;
    stbuf->st_size = strlen(hello_str);
    break;

  default:
    return -1;
  }
  return 0;
}

static void hello_ll_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi){

  struct operation_template operation;

  operation.index = operation_count++;
  operation.type = OP_GETATTR;
  operation.req = &(req);
  operation.ino = ino;
  loop_async.data = &(operation);

  operations_map[ operation.index ] = &(operation);

  uv_async_send(&loop_async);
}

static void hello_ll_lookup(fuse_req_t req, fuse_ino_t parent, const char *name){
  fprintf(stderr, "%s => \n", "lookup");
  struct fuse_entry_param e;

  if (parent != 1 || strcmp(name, hello_name) != 0)
    fuse_reply_err(req, ENOENT);
  else {
    memset(&e, 0, sizeof(e));
    e.ino = 2;
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;
    hello_stat(e.ino, &e.attr);

    fuse_reply_entry(req, &e);
  }
}

struct dirbuf {
  char *p;
  size_t size;
};

static void dirbuf_add(fuse_req_t req, struct dirbuf *b, const char *name, fuse_ino_t ino){
  struct stat stbuf;
  size_t oldsize = b->size;
  b->size += fuse_add_direntry(req, NULL, 0, name, NULL, 0);
  b->p = (char *) realloc(b->p, b->size);
  memset(&stbuf, 0, sizeof(stbuf));
  stbuf.st_ino = ino;
  fuse_add_direntry(req, b->p + oldsize, b->size - oldsize, name, &stbuf,
        b->size);
}

#define min(x, y) ((x) < (y) ? (x) : (y))

static int reply_buf_limited(fuse_req_t req, const char *buf, size_t bufsize, off_t off, size_t maxsize){
  if (off < bufsize)
    return fuse_reply_buf(req, buf + off,
              min(bufsize - off, maxsize));
  else
    return fuse_reply_buf(req, NULL, 0);
}

static void hello_ll_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi){
  fprintf(stderr, "%s\n", "readdir");
  (void) fi;

  if (ino != 1)
    fuse_reply_err(req, ENOTDIR);
  else {
    struct dirbuf b;

    memset(&b, 0, sizeof(b));
    dirbuf_add(req, &b, ".", 1);
    dirbuf_add(req, &b, "..", 1);
    dirbuf_add(req, &b, hello_name, 2);
    reply_buf_limited(req, b.p, b.size, off, size);
    free(b.p);
  }
}

static void hello_ll_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi){
  fprintf(stderr, "%s\n", "open");
  if (ino != 2)
    fuse_reply_err(req, EISDIR);
  else if ((fi->flags & 3) != O_RDONLY)
    fuse_reply_err(req, EACCES);
  else
    fuse_reply_open(req, fi);
}

static void hello_ll_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi){
  fprintf(stderr, "%s\n", "read");
  (void) fi;

  assert(ino == 2);
  reply_buf_limited(req, hello_str, strlen(hello_str), off, size);
}

static struct fuse_lowlevel_ops hello_ll_oper = {};

static void *fuse_thread(void *path){

  /*
  //hello_ll_oper.release = hello_ll_release;
  */
  //char **argv = [ "fuse-bindings", path ];

  /*char** files;
  files = malloc(1 * sizeof(char*));
  files[0] = malloc(255 * sizeof(char));*/

  char *argv[] = {
    (char *) "fuse_bindings_dummy",
    (char *) "mnt/"
  };

  struct fuse_args args = FUSE_ARGS_INIT(2, argv);
  struct fuse_session *se;
  struct fuse_cmdline_opts opts;
  /*
  int ret = -1;

  if (*/fuse_parse_cmdline(&args, &opts);/*!= 0)
    return 1;
  if (opts.show_help) {
    printf("usage: %s [options] <mountpoint>\n\n", argv[0]);
    fuse_cmdline_help();
    fuse_lowlevel_help();
    ret = 0;
    goto err_out1;
  } else if (opts.show_version) {
    printf("FUSE library version %s\n", fuse_pkgversion());
    fuse_lowlevel_version();
    ret = 0;
    goto err_out1;
  }*/

  se = fuse_session_new(&args, &hello_ll_oper, sizeof(hello_ll_oper), NULL);

  /*
  if (se == NULL)
      goto err_out1;
  */

  /*if (*/fuse_set_signal_handlers(se);/* != 0)
      goto err_out2;

  if (*/fuse_session_mount(se, opts.mountpoint);/* != 0)
      goto err_out3;

  */
    fprintf(stderr, "%s\n", "bucle");

  int ret = fuse_session_loop(se);
    fprintf(stderr, "%s\n", "fin bucle");

  
  fuse_session_unmount(se);
  /*

  err_out3: fuse_remove_signal_handlers(se);
  err_out2: fuse_session_destroy(se);
  err_out1: free(opts.mountpoint);
            fuse_opt_free_args(&args);

  return ret ? 1 : 0;
  */
}

NAN_METHOD(SetCallback) {
  callback_constructor = new Nan::Callback(info[0].As<Function>());
}

NAN_METHOD(Mount) {
  fprintf(stderr, "%s => %d\n", "nan mount", std::this_thread::get_id() );

  if(!info[0]->IsString()){
    return Nan::ThrowError("mnt must be a string");
  }

  //ToDo -> Path
  Local<Object> operations = info[1].As<Object>();
  
  bindings.getattr = LOOKUP_CALLBACK(operations, "getattr");
  /*bindings.stat    = LOOKUP_CALLBACK(operations, "stat");
  bindings.readdir = LOOKUP_CALLBACK(operations, "readdir");
  bindings.lookup  = LOOKUP_CALLBACK(operations, "lookup");*/

  loop = uv_default_loop();
  uv_async_init(loop, &loop_async, uv_handler);

  v8::Local<v8::Value> path = info[0];

  pthread_t thread_id;
  pthread_attr_t thread_attr;

  pthread_attr_init(&thread_attr);
  pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_JOINABLE);
  pthread_create(&thread_id, &thread_attr, &fuse_thread, &path);
}

void Init(Handle<Object> exports) {
  hello_ll_oper.lookup  = hello_ll_lookup;
  hello_ll_oper.getattr = hello_ll_getattr;
  hello_ll_oper.open    = hello_ll_open;
  hello_ll_oper.read    = hello_ll_read;
  hello_ll_oper.readdir = hello_ll_readdir;

  exports->Set(LOCAL_STRING("setCallback"), Nan::New<FunctionTemplate>(SetCallback)->GetFunction());
  exports->Set(LOCAL_STRING("mount"), Nan::New<FunctionTemplate>(Mount)->GetFunction());
}

NODE_MODULE(fuse_bindings, Init);
