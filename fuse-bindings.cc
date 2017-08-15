
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

#define min(x, y) ((x) < (y) ? (x) : (y))
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
  OP_DESTROY,
  OP_LOOKUP
};

struct bindings_template {
  Nan::Callback *getattr;
  Nan::Callback *lookup;
  Nan::Callback *open;
  Nan::Callback *read;
  Nan::Callback *readdir;
  Nan::Callback *release;
};

struct operation_template {
  uint32_t index;
  int type;
  fuse_req_t *req;
  fuse_ino_t ino;
  fuse_file_info *fi;
  size_t size;
  off_t offset;
  const char *name;
  char *data;
};

static uv_loop_t *loop;
static uv_async_t loop_async;
static struct bindings_template bindings;
static Nan::Callback *callback_constructor;
static std::unordered_map<std::uint32_t, operation_template *> operations_map = {};
static uint32_t operation_count = 0;
static pthread_mutex_t mutex;
static pthread_mutex_t mutex_ip;

struct dirbuf {
  char *p;
  size_t size;
};

const char* ToConstString(const String::Utf8Value& value) {
  return *value ? *value : "<string conversion failed>";
}

static void dirbuf_add( fuse_req_t req, struct dirbuf *b, const char *name, fuse_ino_t ino ){
  struct stat stbuf;
  size_t oldsize = b->size;
  b->size += fuse_add_direntry(req, NULL, 0, name, NULL, 0);
  b->p = (char *) realloc(b->p, b->size);
  memset(&stbuf, 0, sizeof(stbuf));
  stbuf.st_ino = ino;
  fuse_add_direntry(req, b->p + oldsize, b->size - oldsize, name, &stbuf, b->size);
}

static int reply_buf_limited(fuse_req_t req, const char *buf, size_t bufsize, off_t off, size_t maxsize){

  fprintf(stderr, "%s => bufsize %d - offset %d - maxsize %d\n", "reply_buf_limited", bufsize, off, maxsize);

  if (off < bufsize){
    return fuse_reply_buf(req, buf + off, min(bufsize - off, maxsize));
  }else{
    return fuse_reply_buf(req, NULL, 0);
  }

}

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
  if (obj->Has(LOCAL_STRING("ino"))) stat->st_ino = obj->Get(LOCAL_STRING("ino"))->Uint32Value();
  if (obj->Has(LOCAL_STRING("mode"))) stat->st_mode = obj->Get(LOCAL_STRING("mode"))->Uint32Value();
  if (obj->Has(LOCAL_STRING("nlink"))) stat->st_nlink = obj->Get(LOCAL_STRING("nlink"))->Uint32Value();
  if (obj->Has(LOCAL_STRING("uid"))) stat->st_uid = obj->Get(LOCAL_STRING("uid"))->NumberValue();
  if (obj->Has(LOCAL_STRING("gid"))) stat->st_gid = obj->Get(LOCAL_STRING("gid"))->NumberValue();
  if (obj->Has(LOCAL_STRING("rdev"))) stat->st_rdev = obj->Get(LOCAL_STRING("rdev"))->NumberValue();
  if (obj->Has(LOCAL_STRING("size"))) stat->st_size = obj->Get(LOCAL_STRING("size"))->Uint32Value();
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

#if (NODE_MODULE_VERSION > NODE_0_10_MODULE_VERSION && NODE_MODULE_VERSION < IOJS_3_0_MODULE_VERSION)
NAN_INLINE v8::Local<v8::Object> bindings_buffer (char *data, size_t length) {
  Local<Object> buf = Nan::New(buffer_constructor)->NewInstance(0, NULL);
  Local<String> k = LOCAL_STRING("length");
  Local<Number> v = LOCAL_NUMBER(length);
  buf->Set(k, v);
  buf->SetIndexedPropertiesToExternalArrayData((char *) data, kExternalUnsignedByteArray, length);
  return buf;
}
#else
void noop (char *data, void *hint) {}
NAN_INLINE v8::Local<v8::Object> bindings_buffer (char *data, size_t length) {
  fprintf(stderr, "%s => %d\n", "buffer nuevo", length);
  return Nan::NewBuffer(data, length, noop, NULL).ToLocalChecked();
}
#endif

NAN_METHOD(OpCallback){

  pthread_mutex_lock(&mutex);
  operation_template *operation = operations_map[ info[ 0 ]->Uint32Value() ];
  operations_map.erase( operation->index );
  pthread_mutex_unlock(&mutex);

  int result = (info.Length() > 1 && info[1]->IsNumber()) ? info[1]->Int32Value() : 0;

  if( operation->type == OP_GETATTR ){

    fprintf(stderr, "%s => %d\n", "OP_GETATTR", result );

    if(result == 0 && info.Length() > 2 && info[2]->IsObject()){

      struct stat stbuf;
      memset( &stbuf, 0, sizeof(stbuf) );
      bindings_set_stat( &stbuf, info[2].As<Object>());
      fuse_reply_attr(*(operation->req), &stbuf, 1.0);

    }else{
      fprintf(stderr, "%s %d\n", "get attr falla",ENOENT);
      fuse_reply_err(*(operation->req), ENOENT);
    }

  }else if( operation->type == OP_LOOKUP ){

    fprintf(stderr, "%s => %d\n", "OP_LOOKUP", result );

    if(result == 0 && info.Length() > 2 && info[2]->IsObject()){

      struct fuse_entry_param entry;

      Local<Object> data = info[2].As<Object>();
      uint32_t ino = data->Get(LOCAL_STRING("ino"))->Uint32Value();

      memset(&entry, 0, sizeof(entry));
      entry.ino = ino;
      entry.attr_timeout = 1.0;
      entry.entry_timeout = 1.0;
      bindings_set_stat( &entry.attr, data);
      fuse_reply_entry(*(operation->req), &entry);

    }else{
      fuse_reply_err(*(operation->req), ENOENT);
    }

  }else if( operation->type == OP_OPEN ){

    fprintf(stderr, "%s => %d\n", "OP_OPEN", result );

    if(result == 0 && info.Length() > 2 && info[2]->IsNumber()){
      operation->fi->fh = info[2]->Uint32Value();
      fuse_reply_open(*(operation->req), operation->fi);
    }else{
      fuse_reply_err(*(operation->req), ENOENT);
    }

  }else if( operation->type == OP_READDIR ){

    fprintf(stderr, "%s => %d\n", "OP_READDIR", result );

    if(result == 0 && info.Length() > 2 && info[2]->IsArray()){

      struct dirbuf buf;

      memset(&buf, 0, sizeof(buf));
      dirbuf_add( *(operation->req), &buf, ".", 1);
      dirbuf_add( *(operation->req), &buf, "..", 1);

      Local<Array> entries = info[2].As<Array>();

      Local<Object> entry;
      Local<String> entryName;
      uint32_t entryIno;

      for( uint32_t i = 0; i < entries->Length(); i++ ){

        entry = entries->Get( i )->ToObject();
        entryName = entry->Get(LOCAL_STRING("name"))->ToString();
        entryIno = entry->Get(LOCAL_STRING("ino"))->Uint32Value();

        dirbuf_add( *(operation->req), &buf, ToConstString( String::Utf8Value( entryName ) ), entryIno );

      }

      reply_buf_limited( *(operation->req), buf.p, buf.size, operation->offset, operation->size );
      free(buf.p);

    }else{
      fuse_reply_err(*(operation->req), ENOENT);
    }

  }else if( operation->type == OP_READ ){

    fprintf(stderr, "%s => %d\n", "OP_READ", result );

    if(result >= 0 ){
      reply_buf_limited(*(operation->req), operation->data, result, operation->offset, operation->size);
    }else{
      fuse_reply_err(*(operation->req), ENOENT);
    }

  }else if( operation->type == OP_RELEASE ){
    fprintf(stderr, "%s => %d\n", "OP_RELEASE", result );
    fuse_reply_err(*(operation->req), result == 0 ? 0 : ENOENT);
  }else{
    fprintf(stderr, "OpCallback => %s\n", "not implemented");
  }

}

static void uv_handler(uv_async_t *handle){
  Nan::HandleScope scope;

  operation_template *operation = (operation_template *) handle->data;
  pthread_mutex_unlock(&mutex_ip);
  Local<Value> tmp[] = { Nan::New<Number>(operation->index), Nan::New<FunctionTemplate>(OpCallback)->GetFunction() };
  Nan::Callback *callbackGenerator = new Nan::Callback(callback_constructor->Call(2, tmp).As<Function>());
  Local<Function> callback = callbackGenerator->GetFunction();

  if( operation->type == OP_GETATTR ){

    Local<Value> tmp[] = {LOCAL_NUMBER(operation->ino), callback};
    bindings.getattr->Call( 2, tmp );

  }else if( operation->type == OP_READDIR ){

    Local<Value> tmp[] = {LOCAL_NUMBER(operation->ino), callback};
    bindings.readdir->Call( 2, tmp );

  }else if( operation->type == OP_LOOKUP ){

    Local<Value> tmp[] = {LOCAL_NUMBER(operation->ino), LOCAL_STRING(operation->name), callback};
    bindings.lookup->Call( 3, tmp );

  }else if( operation->type == OP_OPEN ){

    Local<Value> tmp[] = {LOCAL_NUMBER(operation->ino), callback};
    bindings.open->Call( 2, tmp );

  }else if( operation->type == OP_READ ){

    Local<Value> tmp[] = {
      LOCAL_NUMBER(operation->ino),
      LOCAL_NUMBER(operation->fi->fh),
      bindings_buffer( operation->data, operation->size),
      LOCAL_NUMBER(operation->size),
      LOCAL_NUMBER(operation->offset),
      callback
    };
    bindings.read->Call( 6, tmp );

  }else if( operation->type == OP_RELEASE ){

    Local<Value> tmp[] = {LOCAL_NUMBER(operation->ino), LOCAL_NUMBER(operation->fi->fh), callback};
    bindings.release->Call( 3, tmp );

  }else{
    fprintf(stderr, "uv_handler => %s\n", "not implemented");
  }

}

static void ll_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi){
  fprintf(stderr, "%s\n", "getattr");
  struct operation_template operation;

  operation.type = OP_GETATTR;
  operation.req = &(req);
  operation.ino = ino;
  loop_async.data = &(operation);

  pthread_mutex_lock(&mutex);
  operation.index = operation_count++;
  operations_map[ operation.index ] = &(operation);
  pthread_mutex_unlock(&mutex);

  uv_async_send(&loop_async);
  pthread_mutex_lock(&mutex_ip);
}

static void ll_lookup(fuse_req_t req, fuse_ino_t parent, const char *name){
  fprintf(stderr, "%s\n", "lookup");
  struct operation_template operation;

  operation.type = OP_LOOKUP;
  operation.req = &(req);
  operation.ino = parent;
  operation.name = name;
  loop_async.data = &(operation);

  pthread_mutex_lock(&mutex);
  operation.index = operation_count++;
  operations_map[ operation.index ] = &(operation);
  pthread_mutex_unlock(&mutex);

  uv_async_send(&loop_async);
  pthread_mutex_lock(&mutex_ip);
}

static void ll_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi){
  fprintf(stderr, "%s\n", "readdir");
  struct operation_template operation;

  operation.type = OP_READDIR;
  operation.req = &(req);
  operation.size = size;
  operation.offset = off;
  operation.ino = ino;
  operation.fi = fi;
  loop_async.data = &(operation);

  pthread_mutex_lock(&mutex);
  operation.index = operation_count++;
  operations_map[ operation.index ] = &(operation);
  pthread_mutex_unlock(&mutex);

  uv_async_send(&loop_async);
  pthread_mutex_lock(&mutex_ip);
}

static void ll_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi){
  fprintf(stderr, "%s\n", "open");

  struct operation_template operation;

  operation.type = OP_OPEN;
  operation.req = &(req);
  operation.ino = ino;
  operation.fi = fi;
  loop_async.data = &(operation);

  pthread_mutex_lock(&mutex);
  operation.index = operation_count++;
  operations_map[ operation.index ] = &(operation);
  pthread_mutex_unlock(&mutex);

  uv_async_send(&loop_async);
  pthread_mutex_lock(&mutex_ip);
}

static void ll_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi){
  fprintf(stderr, "%s\n", "release");
  struct operation_template operation;

  operation.type = OP_RELEASE;
  operation.req = &(req);
  operation.ino = ino;
  operation.fi = fi;
  loop_async.data = &(operation);

  pthread_mutex_lock(&mutex);
  operation.index = operation_count++;
  operations_map[ operation.index ] = &(operation);
  pthread_mutex_unlock(&mutex);

  uv_async_send(&loop_async);
  pthread_mutex_lock(&mutex_ip);
}

static void ll_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi){
  fprintf(stderr, "%s\n", "read");

  struct operation_template operation;
  char buf[ size ];

  operation.type = OP_READ;
  operation.req = &(req);
  operation.size = size;
  operation.offset = off;
  operation.ino = ino;
  operation.data = buf;
  operation.fi = fi;
  loop_async.data = &(operation);

  pthread_mutex_lock(&mutex);
  operation.index = operation_count++;
  operations_map[ operation.index ] = &(operation);
  pthread_mutex_unlock(&mutex);

  uv_async_send(&loop_async);
  pthread_mutex_lock(&mutex_ip);
}

static struct fuse_lowlevel_ops ll_oper = {};

static void *fuse_thread(void *path){

  /*
  //ll_oper.release = ll_release;
  */
  //char **argv = [ "fuse-bindings", path ];

  /*char** files;
  files = malloc(1 * sizeof(char*));
  files[0] = malloc(255 * sizeof(char));*/

  char *argv[] = {
    (char *) "fuse_bindings_dummy",
    (char *) path
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

  se = fuse_session_new(&args, &ll_oper, sizeof(ll_oper), NULL);

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

  if(!info[0]->IsString()){
    return Nan::ThrowError("mnt must be a string");
  }

  //ToDo -> Path
  Nan::Utf8String path(info[0]);
  Local<Object> operations = info[1].As<Object>();
  char mountpoint[1024];
  strcpy(mountpoint, *path);

  bindings.open    = LOOKUP_CALLBACK(operations, "open");
  bindings.read    = LOOKUP_CALLBACK(operations, "read");
  bindings.getattr = LOOKUP_CALLBACK(operations, "getattr");
  bindings.lookup  = LOOKUP_CALLBACK(operations, "lookup");
  bindings.readdir = LOOKUP_CALLBACK(operations, "readdir");
  bindings.release = LOOKUP_CALLBACK(operations, "release");

  loop = uv_default_loop();
  uv_async_init(loop, &loop_async, uv_handler);

  pthread_t thread_id;
  pthread_attr_t thread_attr;

  pthread_mutex_init(&mutex, NULL);
  pthread_mutex_init(&mutex_ip, NULL);
  pthread_attr_init(&thread_attr);
  pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_JOINABLE);
  pthread_create(&thread_id, &thread_attr, &fuse_thread, &mountpoint);
}

void Init(Handle<Object> exports) {
  ll_oper.getattr = ll_getattr;
  ll_oper.lookup  = ll_lookup;
  ll_oper.open    = ll_open;
  ll_oper.read    = ll_read;
  ll_oper.readdir = ll_readdir;
  ll_oper.release = ll_release;

  exports->Set(LOCAL_STRING("setCallback"), Nan::New<FunctionTemplate>(SetCallback)->GetFunction());
  exports->Set(LOCAL_STRING("mount"), Nan::New<FunctionTemplate>(Mount)->GetFunction());
}

NODE_MODULE(fuse_bindings, Init);
