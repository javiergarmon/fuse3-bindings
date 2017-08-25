
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
  OP_TRUNCATE, //10
  OP_FTRUNCATE,
  OP_UTIMENS,
  OP_READLINK,
  OP_CHOWN,
  OP_CHMOD,
  OP_MKNOD,
  OP_SETXATTR,
  OP_GETXATTR,
  OP_LISTXATTR,
  OP_REMOVEXATTR, // 20
  OP_OPEN,
  OP_OPENDIR,
  OP_READ,
  OP_WRITE,
  OP_RELEASE,
  OP_RELEASEDIR,
  OP_CREATE,
  OP_UNLINK,
  OP_RENAME,
  OP_LINK, // 30
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
  unsigned long index;
  int type;
  fuse_req_t req;
  fuse_ino_t ino;
  fuse_file_info fi;
  unsigned long fd;
  size_t size;
  off_t offset;
  const char *name;
  char *data;
};

static uv_loop_t *loop;
static uv_async_t loop_async;
static struct bindings_template bindings;
static Nan::Callback *callback_constructor;
static Nan::Callback *commonCallback;
static std::unordered_map<unsigned long, operation_template *> operations_map = {};
static unsigned long operation_count = 0;
static pthread_mutex_t mutex;
static sem_t *sem_ip;

struct dirbuf {
  char *p;
  size_t size;
};

NAN_INLINE const char* ToConstString(const String::Utf8Value& value) {
  return *value ? *value : "<string conversion failed>";
}

NAN_INLINE static void dirbuf_add( fuse_req_t req, struct dirbuf *b, const char *name, fuse_ino_t ino ){
  struct stat stbuf;
  size_t oldsize = b->size;
  b->size += fuse_add_direntry(req, NULL, 0, name, NULL, 0);
  b->p = (char *) realloc(b->p, b->size);
  memset(&stbuf, 0, sizeof(stbuf));
  stbuf.st_ino = ino;
  fuse_add_direntry(req, b->p + oldsize, b->size - oldsize, name, &stbuf, b->size);
}

NAN_INLINE static int reply_buf_limited(fuse_req_t req, const char *buf, size_t bufsize, off_t off, size_t maxsize){

  //fprintf(stderr, "%s => bufsize %ld - offset %ld - maxsize %ld\n", "reply_buf_limited", (int) bufsize, (int) off, (int) maxsize);

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
  if (obj->Has(LOCAL_STRING("ino"))) stat->st_ino = obj->Get(LOCAL_STRING("ino"))->NumberValue();
  if (obj->Has(LOCAL_STRING("mode"))) stat->st_mode = obj->Get(LOCAL_STRING("mode"))->Uint32Value();
  if (obj->Has(LOCAL_STRING("nlink"))) stat->st_nlink = obj->Get(LOCAL_STRING("nlink"))->Uint32Value();
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
  //fprintf(stderr, "%s => %ld\n", "buffer nuevo", (int) length);
  return Nan::NewBuffer(data, length, noop, NULL).ToLocalChecked();
}
#endif

NAN_METHOD(OpCallback){

  pthread_mutex_lock(&mutex);
  fprintf(stderr, "Extraigo %ld\n", info[ 0 ]->NumberValue());
  operation_template *operation = operations_map[ info[ 0 ]->NumberValue() ];
  operations_map.erase( operation->index );
  pthread_mutex_unlock(&mutex);

  fprintf(stderr, "REPLY(%ld) => dir req( %p )\n", operation->index, operation->req);

  int result = (info.Length() > 1 && info[1]->IsNumber()) ? info[1]->Int32Value() : 0;

  if( operation->type == OP_GETATTR ){

    //fprintf(stderr, "%s => %ld\n", "OP_GETATTR", result );

    if(result == 0 && info.Length() > 2 && info[2]->IsObject()){

      struct stat stbuf;
      memset( &stbuf, 0, sizeof(stbuf) );
      bindings_set_stat( &stbuf, info[2].As<Object>());
      fuse_reply_attr(operation->req, &stbuf, 10.0);

    }else{
      //fprintf(stderr, "%s %ld\n", "get attr falla",ENOENT);
      fuse_reply_err(operation->req, ENOENT);
    }

  }else if( operation->type == OP_LOOKUP ){

    //fprintf(stderr, "%s => %ld\n", "OP_LOOKUP", result );

    if(result == 0 && info.Length() > 2 && info[2]->IsObject()){

      struct fuse_entry_param entry;

      Local<Object> data = info[2].As<Object>();
      unsigned long ino = data->Get(LOCAL_STRING("ino"))->NumberValue();

      memset(&entry, 0, sizeof(entry));
      entry.ino = ino;
      entry.attr_timeout = 10.0;
      entry.entry_timeout = 10.0;
      bindings_set_stat( &entry.attr, data);
      fuse_reply_entry(operation->req, &entry);
      delete operation->name;

    }else{
      fuse_reply_err(operation->req, ENOENT);
    }

  }else if( operation->type == OP_OPEN ){

    //fprintf(stderr, "%s => %ld\n", "OP_OPEN", result );

    if(result == 0 && info.Length() > 2 && info[2]->IsNumber()){
      operation->fi.fh = info[2]->NumberValue();
      fuse_reply_open(operation->req, &operation->fi);
    }else{
      fuse_reply_err(operation->req, ENOENT);
    }

  }else if( operation->type == OP_READDIR ){

    //fprintf(stderr, "%s => %ld\n", "OP_READDIR", result );

    if(result == 0 && info.Length() > 2 && info[2]->IsArray()){

      struct dirbuf buf;

      memset(&buf, 0, sizeof(buf));
      dirbuf_add( operation->req, &buf, ".", 1);
      dirbuf_add( operation->req, &buf, "..", 1);

      Local<Array> entries = info[2].As<Array>();

      Local<Object> entry;
      Local<String> entryName;
      unsigned long entryIno;

      for( uint32_t i = 0; i < entries->Length(); i++ ){

        entry = entries->Get( i )->ToObject();
        entryName = entry->Get(LOCAL_STRING("name"))->ToString();
        entryIno = entry->Get(LOCAL_STRING("ino"))->NumberValue();

        dirbuf_add( operation->req, &buf, ToConstString( String::Utf8Value( entryName ) ), entryIno );

      }

      reply_buf_limited( operation->req, buf.p, buf.size, operation->offset, operation->size );
      free(buf.p);

    }else{
      fuse_reply_err(operation->req, ENOENT);
    }

  }else if( operation->type == OP_READ ){
    //fprintf(stderr, "%s => %ld\n", "OP_READ", result );

    if(result >= 0 ){
      fprintf(stderr, "op(%ld) req(%p) buffer(%p) size(%ld)\n", operation->index, operation->req, &operation->data, operation->size);
      fuse_reply_buf( operation->req, operation->data, operation->size );
      delete operation->data;
      //reply_buf_limited(operation->req, operation->data, result, operation->offset, operation->size);
    }else{
      //fprintf(stderr, "%s => %ld\n", "Respondiendo con error", ENOENT);
      fuse_reply_err(operation->req, ENOENT);
    }

  }else if( operation->type == OP_RELEASE ){
    //fprintf(stderr, "%s => %ld\n", "OP_RELEASE", result );
    fuse_reply_err(operation->req, result == 0 ? 0 : ENOENT);
  }else{
    fprintf(stderr, "OpCallback => %s - %ld - %ld\n", "not implemented",operation->type,result);
  }

  fprintf(stderr, "%s => %p\n", "OpCallback END", operation->req);
  delete operation;

}

void uv_handler(uv_async_t *handle){
  Nan::HandleScope scope;

  operation_template *operation = (operation_template *) handle->data;
  fprintf(stderr, "uv_handler %p\n", operation->req);
  //fprintf(stderr, "UNLOCK SEM\n");
  sem_post(sem_ip);
  Local<Function> callback = commonCallback->GetFunction();

  if( operation->type == OP_GETATTR ){

    Local<Value> tmp[] = {LOCAL_NUMBER(operation->index), LOCAL_NUMBER(operation->ino), LOCAL_NUMBER(operation->fd), callback};
    bindings.getattr->Call( 4, tmp );

  }else if( operation->type == OP_READDIR ){

    Local<Value> tmp[] = {LOCAL_NUMBER(operation->index), LOCAL_NUMBER(operation->ino), callback};
    bindings.readdir->Call( 3, tmp );

  }else if( operation->type == OP_LOOKUP ){

    Local<Value> tmp[] = {LOCAL_NUMBER(operation->index), LOCAL_NUMBER(operation->ino), LOCAL_STRING(operation->name), callback};
    bindings.lookup->Call( 4, tmp );

  }else if( operation->type == OP_OPEN ){

    Local<Value> tmp[] = {LOCAL_NUMBER(operation->index), LOCAL_NUMBER(operation->ino), callback};
    bindings.open->Call( 3, tmp );

  }else if( operation->type == OP_READ ){

    fprintf(stderr, "Pidiendo(%ld) al fd(%ld) size(%ld) con offset(%ld)\n", operation->index, operation->fd, operation->size, operation->offset);
    fprintf(stderr, "Alojando en buffer %p\n", operation->data);

    Local<Value> tmp[] = {
      LOCAL_NUMBER(operation->index),
      LOCAL_NUMBER(operation->ino),
      LOCAL_NUMBER(operation->fd),
      bindings_buffer( operation->data, operation->size),
      LOCAL_NUMBER(operation->size),
      LOCAL_NUMBER(operation->offset),
      callback
    };
    bindings.read->Call( 7, tmp );

  }else if( operation->type == OP_RELEASE ){

    Local<Value> tmp[] = {LOCAL_NUMBER(operation->index), LOCAL_NUMBER(operation->ino), LOCAL_NUMBER(operation->fd), callback};
    bindings.release->Call( 4, tmp );

  }else{
    fprintf(stderr, "uv_handler => %s %ld\n", "not implemented",operation->type);
  }

}

void ll_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi){
  //fprintf(stderr, "%s\n", "getattr");
  operation_template* operation = new operation_template();

  operation->type = OP_GETATTR;
  operation->req = req;
  operation->ino = ino;

  if( fi == NULL ){
    operation->fd = -1;
  }else{
    operation->fd = fi->fh;
  }

  loop_async.data = operation;

  pthread_mutex_lock(&mutex);
  operation->index = operation_count++;
  operations_map[ operation->index ] = operation;
  pthread_mutex_unlock(&mutex);

  fprintf(stderr, "getattr(%ld) => dir req( %p )\n", operation->index, req);

  uv_async_send(&loop_async);
  //fprintf(stderr, "LOCK SEM\n");
  sem_wait(sem_ip);
}

void ll_lookup(fuse_req_t req, fuse_ino_t parent, const char *name){
  //fprintf(stderr, "%s\n", "lookup");
  operation_template* operation = new operation_template();

  char* name_copy = new char[strlen( name ) ];
  strcpy(name_copy,name);

  operation->type = OP_LOOKUP;
  operation->req = req;
  operation->ino = parent;
  operation->name = name_copy;
  loop_async.data = operation;

  pthread_mutex_lock(&mutex);
  operation->index = operation_count++;
  operations_map[ operation->index ] = operation;
  pthread_mutex_unlock(&mutex);

  uv_async_send(&loop_async);
  //fprintf(stderr, "LOCK SEM\n");
  sem_wait(sem_ip);
}

void ll_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi){
  //fprintf(stderr, "%s\n", "readdir");
  operation_template* operation = new operation_template();

  operation->type = OP_READDIR;
  operation->req = req;
  operation->size = size;
  operation->offset = off;
  operation->ino = ino;
  operation->fd = fi->fh;
  loop_async.data = operation;

  pthread_mutex_lock(&mutex);
  operation->index = operation_count++;
  operations_map[ operation->index ] = operation;
  pthread_mutex_unlock(&mutex);

  uv_async_send(&loop_async);
  //fprintf(stderr, "LOCK SEM\n");
  sem_wait(sem_ip);
}

void ll_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi){
  //fprintf(stderr, "%s\n", "open");
  operation_template* operation = new operation_template();

  fuse_file_info copy_fi;
  copy_fi.flags = fi->flags;
  copy_fi.writepage = fi->writepage;
  copy_fi.direct_io = fi->direct_io;
  copy_fi.keep_cache = fi->keep_cache;
  copy_fi.flush = fi->flush;
  copy_fi.nonseekable = fi->nonseekable;
  copy_fi.padding = fi->padding;
  copy_fi.fh = fi->fh;
  copy_fi.lock_owner = fi->lock_owner;
  copy_fi.poll_events = fi->poll_events;

  operation->type = OP_OPEN;
  operation->req = req;
  operation->ino = ino;
  operation->fi = copy_fi;
  loop_async.data = operation;

  pthread_mutex_lock(&mutex);
  operation->index = operation_count++;
  operations_map[ operation->index ] = operation;
  pthread_mutex_unlock(&mutex);

  uv_async_send(&loop_async);
  //fprintf(stderr, "LOCK SEM\n");
  sem_wait(sem_ip);
}

void ll_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi){
  //fprintf(stderr, "%s\n", "release");
  operation_template* operation = new operation_template();

  operation->type = OP_RELEASE;
  operation->req = req;
  operation->ino = ino;
  operation->fd = fi->fh;
  loop_async.data = operation;

  pthread_mutex_lock(&mutex);
  operation->index = operation_count++;
  operations_map[ operation->index ] = operation;
  pthread_mutex_unlock(&mutex);

  uv_async_send(&loop_async);
  //fprintf(stderr, "LOCK SEM\n");
  sem_wait(sem_ip);
}

void ll_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi){
  //fprintf(stderr, "%s\n", "read");
  operation_template* operation = new operation_template();
  //struct operation_template operation;
  //memset( &operation, 0, sizeof(operation) );
  //char buf[ size ]; // struct fuse_bufvec buf = FUSE_BUFVEC_INIT(size);
  char* buf = new char[size];

  fprintf(stderr, "BUFFER %p\n", buf);

  operation->type = OP_READ;
  operation->req = req;
  operation->size = size;
  operation->offset = off;
  operation->ino = ino;
  operation->data = buf;
  operation->fd = fi->fh;
  loop_async.data = operation;

  pthread_mutex_lock(&mutex);
  operation->index = operation_count++;
  operations_map[ operation->index ] = operation;
  pthread_mutex_unlock(&mutex);

  fprintf(stderr, "read(%ld) => dir req( %p )\n", operation->index, req);

  uv_async_send(&loop_async);
  //fprintf(stderr, "LOCK SEM\n");
  sem_wait(sem_ip);
}

static void ll_init(void *userdata,struct fuse_conn_info *conn){
  conn->want &= ~FUSE_CAP_ASYNC_READ;
}

static struct fuse_lowlevel_ops ll_oper = {};

static void *fuse_thread(void *path){

  sem_ip = new sem_t();
  sem_init(sem_ip, 0, 0);

  /*
  //ll_oper.release = ll_release;
  */
  //char **argv = [ "fuse-bindings", path ];

  /*char** files;
  files = malloc(1 * sizeof(char*));
  files[0] = malloc(255 * sizeof(char));*/

  char *argv[] = {
    (char *) "fuse_bindings_dummy",
    (char *) "/mnt/flashfs" //path
  };

  //fprintf(stderr, "Mounted at %s\n", path);

  struct fuse_args args = FUSE_ARGS_INIT(2, argv);
  struct fuse_session *se;
  struct fuse_cmdline_opts opts;
  /*
  int ret = -1;

  if( */fprintf(stderr, "%ld\n", fuse_parse_cmdline(&args, &opts) );/*!= 0)
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
    //fprintf(stderr, "%s\n", "bucle");

  /*int ret = */fuse_session_loop(se);
    //fprintf(stderr, "%s\n", "fin bucle");


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

  Local<Value> tmp[] = {Nan::New<FunctionTemplate>(OpCallback)->GetFunction()};
  commonCallback = new Nan::Callback(callback_constructor->Call(1, tmp).As<Function>());

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
  uv_async_init(loop, &loop_async, (uv_async_cb) uv_handler);

  pthread_t thread_id;
  pthread_attr_t thread_attr;

  //sem_ip = new sem_t();

  pthread_mutex_init(&mutex, NULL);
  //sem_init(sem_ip, 0, 0);
  pthread_attr_init(&thread_attr);
  pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED);
  pthread_create(&thread_id, &thread_attr, &fuse_thread, &mountpoint);
}

void Init(Handle<Object> exports) {
  ll_oper.init    = ll_init;
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
