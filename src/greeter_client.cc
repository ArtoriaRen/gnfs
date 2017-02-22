#define FUSE_USE_VERSION 30


#include <fuse.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <assert.h>
#include <sys/stat.h>//open
#include <iostream>
#include <memory>
#include <string>
#include <cstring>
#include <thread> //this_thread::sleep_for
#include <chrono> //chrono::seconds
#include <unordered_map> //

#include <grpc++/grpc++.h>
#include <grpc++/impl/codegen/status_code_enum.h> //DEADLINE_EXCEEDED

#include "helloworld.grpc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using grpc::ClientReader;
using grpc::DEADLINE_EXCEEDED;
using helloworld::HelloRequest;
using helloworld::HelloReply;
using helloworld::Greeter;
using helloworld::Path;
using helloworld::Stbuf;
using helloworld::Request;
using helloworld::Errno;
using helloworld::Directory;
using helloworld::WriteRequest;
using helloworld::WriteBytes;
using helloworld::PathFlags;
using helloworld::FileHandle;
using helloworld::ReadReq;
using helloworld::Buffer;
using helloworld::FlushReq;
using helloworld::RenameReq;
using helloworld::ReleaseReq;
using helloworld::CreateReq;
using helloworld::UtimeReq;

static unsigned long int seq=1; //release(commit) req sequence num
typedef struct {
    const char* buf;
    size_t size;
    off_t offset;
} wdata;
static std::unordered_map<uint64_t, std::list<wdata> > data2write;

class GreeterClient {
    public:
        GreeterClient(std::shared_ptr<Channel> channel)
            : stub_(Greeter::NewStub(channel)) {}

        int grpc_create(const char* path, mode_t mode, struct fuse_file_info *fi) {
            ClientContext context;
            CreateReq req;
            req.set_path(path);
            req.set_mode(mode);
            req.set_flag(fi->flags);
            FileHandle file_handle;

            Status s = stub_->grpc_create(&context, req, &file_handle);
            fi->fh = file_handle.fh();
            std::cout<<"------------------------in grpc_create(), file handle returned by server="<<fi->fh<<std::endl;
            return file_handle.err();
        }

        int grpc_utimens(const char* path, const struct timespec time[2]) 
        {
            ClientContext context;
            UtimeReq req;
            req.set_path(path);
            req.set_at(time[0].tv_sec);
            req.set_mt(time[0].tv_sec);
            Errno err;

            Status s = stub_->grpc_utimens(&context, req, &err);
            return err.err();
        }

        int grpc_mkdir(const char *path, mode_t mode)
        {
            ClientContext context;
            Request req;
            req.set_path(path);
            req.set_mode(mode);
            Errno err;

            // The actual RPC.  
            Status status = stub_->grpc_mkdir(&context, req, &err);
            std::cout<<err.err()<<std::endl;
            // Act upon its status.
            if (status.ok()) {
                return 0;
            } else {
                std::cout << status.error_code() << ": " << status.error_message()
                    << std::endl;
                return err.err();
            }
        }

        int grpc_getattr(const char *client_path, struct stat *statbuf)
        {
            ClientContext context;
            Path pathName;
            pathName.set_path(client_path);
            Stbuf stbuf;

            // The actual RPC.  
            Status status = stub_->grpc_getattr(&context, pathName, &stbuf);
            //tolerate server failure
            printf("-------------Client: %s, status.error_code()= %d ------------\n",__FUNCTION__, status.error_code());
            if(status.error_code()==14){ 
                int i=0;
                for (i=0; i<5; i++){ //retry 5 times
                    std::cout<<"---------------wait for 2s, then resend-------------"<<std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(2)); 
                    ClientContext context_resend;
                    Status status_resend = stub_->grpc_getattr(&context_resend, pathName, &stbuf);
                    if (status_resend.ok())
                        break;  
                }
                if (i==4){
                    std::cout<<"Timeout: After 5 times REread, still failed to getattr."<<std::endl;
                    return 1;
                }
            }


            memset(statbuf, 0, sizeof(struct stat));

            statbuf->st_mode = stbuf.stmode();
            statbuf->st_nlink = stbuf.stnlink();
            statbuf->st_size = stbuf.stsize();

            std::cout<<"stbuf.err()="<<stbuf.err()<<std::endl;
            return stbuf.err();
            //return -2;
        }

        int grpc_unlink(const char *path) {
            ClientContext context;
            Path pathname;
            pathname.set_path(path);
            Errno err;

            Status s = stub_->grpc_unlink(&context, pathname, &err);
            std::cout<<"unlink err()="<<err.err()<<std::endl;
            //if(status.ok()){
            return err.err();
            //}
        }

        int grpc_readdir(const char *client_path, void *buf, fuse_fill_dir_t filler)
        {
            ClientContext context;
            Path pathName;
            pathName.set_path(client_path);
            Directory directory;

            std::unique_ptr<ClientReader<Directory> >reader(
                    stub_->grpc_readdir(&context, pathName));
            while (reader->Read(&directory)){
                struct stat st;
                memset(&st, 0, sizeof(st));
                st.st_ino = directory.dino();
                st.st_mode = directory.dtype() << 12;
                if (filler(buf, directory.dname().c_str(), &st, 0, static_cast<fuse_fill_dir_flags>(0)))
                    break; 
            }

            Status status = reader->Finish();
            // if (status.ok()) {
            //    std::cout << "readdir rpc succeeded." << std::endl;
            //    return 0;
            //  } else {
            //    std::cout << "readdir rpc failed." << std::endl;
            return directory.err();
            // }
        }

        int grpc_write(const char * path, const char* buffer, size_t size, off_t offset, struct fuse_file_info *fi, int resend) {
            if (resend==0){
                //buffer the data to write until grpc_release() remove them from the buffer
                wdata data2buffer;
                data2buffer.buf=buffer;
                data2buffer.size=size;
                data2buffer.offset=offset;
                data2write[fi->fh].push_back(data2buffer);
            }

            ClientContext context;
            // Set timeout for API, Connection timeout in seconds
            gpr_timespec timeOut;
            timeOut.tv_sec=6; //6s
            timeOut.tv_nsec=0;
            timeOut.clock_type=GPR_TIMESPAN;
            context.set_deadline(timeOut);

            WriteRequest req;
            req.set_path(path);
            req.set_buffer(buffer);
            req.set_size(size);
            req.set_offset(offset);
            req.set_fh(fi->fh);
            WriteBytes nbytes;


            std::cout<<"===================write starts============="<<std::endl;
            Status s = stub_->grpc_write(&context, req, &nbytes);
            std::cout<<"===================write ends============="<<std::endl;
            std::cout<<"------------------------status.error_code()="<<s.error_code()<<std::endl;
            if(s.error_code()==DEADLINE_EXCEEDED|| s.error_code()==14){//timeout
                int i=0;
                for (i=0; i<5; i++){ //retry 5 times
                    ClientContext context_rewrite;
                    std::cout<<"===================REwrite starts============="<<std::endl;
                    Status s_rewrite = stub_->grpc_write(&context_rewrite, req, &nbytes);
                    if (s.ok()||s_rewrite.ok())
                        break;
                    std::cout<<"===================REwrite ends============="<<std::endl;
                    std::cout<<"---------------wait for 3s-------------"<<std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(3)); 
                }
                if (i==4){
                    std::cout<<"Timeout: After 5 times REread, still failed to write the file."<<std::endl;
                    return 1;
                }
            }

            if (!nbytes.err()){//no error happens
                fi->fh=nbytes.fh(); // in case of server crash, save the new file handle returned by sever 
                return nbytes.nbytes();
            }else{
                return nbytes.err();
            }

        }

        int grpc_flush(const char* path, struct fuse_file_info *fi) 
        {
            ClientContext context;
            FlushReq req;
            req.set_path(path);
            req.set_fh(fi->fh);
            Errno err;

            Status s = stub_->grpc_flush(&context, req, &err);
            if(s.ok()) {
                std::cout << "flush rpc succeeded." << std::endl;
            } else {
                std::cout << "flush rpc failed." << std::endl;
                return err.err();
            }
            return 0;
        }

        int grpc_open(const char *client_path, struct fuse_file_info *fi)
        {
            ClientContext context;
            PathFlags path_flags;
            path_flags.set_path(client_path);
            path_flags.set_flags(fi->flags);
            FileHandle file_handle;
            Status status = stub_->grpc_open(&context, path_flags, &file_handle);
            //fh=file_handle.fh();
            fi->fh = file_handle.fh();
            return file_handle.err();
        }

        int grpc_read(const char *client_path, char *buf, size_t size, off_t offset, uint64_t* fh)
        {

            ClientContext context;
            // Set timeout for API, Connection timeout in seconds
            gpr_timespec timeOut;
            timeOut.tv_sec=6; //6s
            timeOut.tv_nsec=0;
            timeOut.clock_type=GPR_TIMESPAN;
            context.set_deadline(timeOut);

            ReadReq read_req;
            read_req.set_path(client_path);
            read_req.set_size(size);
            read_req.set_offset(offset);
            read_req.set_fh(*fh);
            Buffer buffer;
            std::cout<<"===================read starts============="<<std::endl;
            Status status = stub_->grpc_read(&context, read_req, &buffer);
            std::cout<<"===================read ends============="<<std::endl;

            std::cout<<"------------------------status.error_code()="<<status.error_code()<<std::endl;
            if(status.error_code()==DEADLINE_EXCEEDED || status.error_code()==14){ //timeout or server failure
                int i=0;
                for (i=0; i<5; i++){ //retry 5 times
                    ClientContext context_reread;
                    std::cout<<"===================REread starts============="<<std::endl;
                    Status status_reread = stub_->grpc_read(&context_reread, read_req, &buffer);
                    if (status.ok()||status_reread.ok())
                        break;
                    std::cout<<"===================REread ends============="<<std::endl;
                    std::cout<<"---------------wait for 3s-------------"<<std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(3)); 
                }
                if (i==4){
                    std::cout<<"Timeout: After 5 times REread, still failed to read the file."<<std::endl;
                    return 1;
                }
            }

            if (!buffer.err()){//no error happens
                *fh=buffer.fh(); // in case of server crash, save the new file handle returned by sever 
                std::string buf_string(buffer.buffer());
                std::strcpy(buf, buf_string.c_str());
                return buffer.nbytes();
            }else{
                return buffer.err();
            }
        }

        int grpc_rename(const char *from, const char *to, unsigned int flags)
        {
            ClientContext context;
            RenameReq rename_req;
            rename_req.set_from(from);
            rename_req.set_to(to);
            rename_req.set_flags(flags);
            Errno err;
            Status status = stub_->grpc_rename(&context, rename_req, &err);
            return err.err();
        }

        int grpc_rmdir(const char *path)
        {
            ClientContext context;
            Path client_path;
            client_path.set_path(path);
            Errno err;
            Status status = stub_->grpc_rmdir(&context, client_path, &err);
            std::cout<<"rmdir: err.err()="<<err.err()<<std::endl;
            return err.err();
        }

        int grpc_release(const char *path, struct fuse_file_info *fi) 
        {
            ClientContext context;
            // Set timeout for API, Connection timeout in seconds
            gpr_timespec timeOut;
            timeOut.tv_sec=6; //6s
            timeOut.tv_nsec=0;
            timeOut.clock_type=GPR_TIMESPAN;
            context.set_deadline(timeOut);

            ReleaseReq req;
            req.set_path(path);
            req.set_fh(fi->fh);
            req.set_seq(seq++);
            FileHandle fd;

            //print data buffered by client
            for(std::list<wdata>::iterator iter=data2write[fi->fh].begin();
                    iter!= data2write[fi->fh].end(); iter++)
            {
                std::cout<<"---------------grpc_release(): data to write="<<iter->buf<<std::endl;
            }

            std::cout<<"===================release starts============="<<std::endl;
            Status s = stub_->grpc_release(&context, req, &fd);
            std::cout<<"===================release ends============="<<std::endl;

            //timeout
            if(s.error_code()==DEADLINE_EXCEEDED){ 
                int i=0;
                for (i=0; i<5; i++){ //retry 5 times
                    ClientContext context_retry;
                    std::cout<<"===================timeout: RErelease starts============="<<std::endl;
                    Status s_retry = stub_->grpc_release(&context_retry, req, &fd);
                    if (s_retry.ok()){
                        std::cout<<"---------------timeout: RErelease succeed"<<std::endl;
                        data2write.erase(fi->fh);//fsync() succeed, remove data from buffer
                        break;
                    }
                    std::cout<<"===================timeout: RErelease ends============="<<std::endl;
                    std::cout<<"---------------wait for 3s-------------"<<std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(3)); 
                }
                if (i==4){
                    std::cout<<"Timeout: After 5 times RErelease, still failed to release the file."<<std::endl;
                    return 1;
                }
            }

            //server failure
            if (s.ok()){//fsync() succeed, remove data from buffer
                std::cout<<"---------------grpc_release() succeed"<<std::endl;
                data2write.erase(fi->fh);
            }else{//fsync() fail, resend
                std::cout<<"---------------grpc_release() fail"<<std::endl;
                uint64_t old_fh=fi->fh; //use the old file handle as key in buffer
                Status s_resend;
                int i=0;
                for (i=0; i<5; i++){
                    //use the file handle returned by server
                    fi->fh=fd.fh(); //for calling grpc_write()
                    req.set_fh(fd.fh());  //for resend grpc_release()
                    //rewrite all data regarding this file handle
                    for(std::list<wdata>::iterator iter=data2write[old_fh].begin();
                            iter!= data2write[old_fh].end(); iter++)
                    {
                        std::cout<<"---------------call grpc_write(), data to write="<<iter->buf<<std::endl;
                        this->grpc_write(path, iter->buf, iter->size, iter->offset, fi, 1); //1 means this is a resend
                    }
                    std::cout<<"---------------send release req again"<<std::endl;
                    ClientContext context_resend;
                    std::cout<<"************send to server file handle= "<<req.fh()<<std::endl;
                    s_resend = stub_->grpc_release(&context_resend, req, &fd);
                    if (s_resend.ok()){
                        std::cout<<"---------------release succeed"<<std::endl;
                        data2write.erase(old_fh);  //resend release req succeed, erase the buffered data
                        break;
                    }
                    std::cout<<"---------------wait for 3s-------------"<<std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(3)); 
                }
                if (i==4){
                    std::cout<<"Timeout: After 5 times resend release req, still failed to release the file."<<std::endl;
                    return 1; 
                }
            }

            return fd.err();
        }

    private:
        std::unique_ptr<Greeter::Stub> stub_;
};


/*
 * Command line options
 *
 * We can't set default values for the char* fields here because
 * fuse_opt_parse would attempt to free() them when the user specifies
 * different values on the command line.
 */
static struct options {
    const char *filename;
    const char *contents;
    GreeterClient *greeter;
    int show_help;
} options;

#define OPTION(t, p)                           \
{ t, offsetof(struct options, p), 1 }
static const struct fuse_opt option_spec[] = {
    OPTION("--name=%s", filename),
    OPTION("--contents=%s", contents),
    OPTION("-h", show_help),
    OPTION("--help", show_help),
    FUSE_OPT_END
};

static void *hello_init(struct fuse_conn_info *conn,
        struct fuse_config *cfg)
{
    (void) conn;
    cfg->kernel_cache = 1;
    return NULL;
}

static int grpc_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
        off_t offset, struct fuse_file_info *fi,
        enum fuse_readdir_flags flags)
{
    (void) offset;
    (void) fi;
    (void) flags;
    return options.greeter->grpc_readdir(path, buf, filler);
}

static int grpc_open(const char *path, struct fuse_file_info *fi)
{
    //	return options.greeter->grpc_open(path, fi->flags, fi->fh);
    printf("===============before call grpc_open(), file heandle = %d \n", fi->fh);
    return options.greeter->grpc_open(path, fi);
    printf("===============after call grpc_open(), file heandle = %d \n", fi->fh);
}

static int grpc_read(const char *path, char *buf, size_t size, off_t offset,
        struct fuse_file_info *fi)
{
    //in case of server crash during call grpc_read(), file handler changes.
    printf("===============before call grpc_read(), file heandle = %d \n", fi->fh);
    int ret=options.greeter->grpc_read(path, buf, size, offset, &(fi->fh));
    printf("===============after call grpc_read(), file heandle = %d \n", fi->fh);
    return ret;
}

static int grpc_write(const char* path, const char* buffer, size_t size, off_t offset, struct fuse_file_info *fi) 
{
    //in case of server crash during call grpc_write(), file handler changes.
    printf("===============before call grpc_write(), file heandle = %d \n", fi->fh);
    int ret=options.greeter->grpc_write(path, buffer, size, offset, fi, 0); //0 means this is not a resend
    printf("===============before call grpc_write(), file heandle = %d \n", fi->fh);
    return ret;
}

static int grpc_getattr(const char *path, struct stat *statbuf,struct fuse_file_info *fi)
{
    (void) fi;
    return options.greeter->grpc_getattr(path, statbuf);
} 

static int grpc_mkdir(const char *path, mode_t mode)
{
    //mode=S_IRWXU | S_IRWXG | S_IRWXO
    return options.greeter->grpc_mkdir(path, mode);
}

static int grpc_unlink(const char* path) {
    return options.greeter->grpc_unlink(path);
}

static int grpc_flush(const char* path, struct fuse_file_info *fi) 
{
    return options.greeter->grpc_flush(path, fi);
}

static int grpc_rename(const char *from, const char *to, unsigned int flags) 
{
    return options.greeter->grpc_rename(from,to, flags);
}

static int grpc_rmdir(const char *path) 
{
    return options.greeter->grpc_rmdir(path);
}

static int grpc_release(const char *path, struct fuse_file_info *fi) 
{
    return options.greeter->grpc_release(path, fi);
}

static int grpc_create(const char* path, mode_t mode, struct fuse_file_info *fi) {
    printf("===============before call grpc_create(), file heandle = %d \n", fi->fh);
    return options.greeter->grpc_create(path, mode, fi);
    printf("===============after call grpc_create(), file heandle = %d \n", fi->fh);
}

static int grpc_utimens(const char* path, const struct timespec time[2], struct fuse_file_info *fi) {
    return options.greeter->grpc_utimens(path, time);
}

static struct hello_operations : fuse_operations {
    hello_operations() {
        init    = hello_init;
        getattr	= grpc_getattr;//hello_getattr;//grpc_getattr;
        readdir	= grpc_readdir; //hello_readdir;
        open	= grpc_open; //hello_open;
        read	= grpc_read; //hello_read;
        mkdir	= grpc_mkdir;
        unlink  = grpc_unlink;
        write   = grpc_write;
        flush   = grpc_flush;
        rename  = grpc_rename;
        rmdir   = grpc_rmdir;
        release = grpc_release;
        create  = grpc_create;
        utimens   = grpc_utimens;
    }
} hello_oper_init;

static void show_help(const char *progname)
{
    std::cout<<"usage: "<<progname<<" [options] <mountpoint>\n\n";
    /*printf("File-system specific options:\n"
      "    --name=<s>          Name of the \"hello\" file\n"
      "                        (default: \"hello\")\n"
      "    --contents=<s>      Contents \"hello\" file\n"
      "                        (default \"Hello, World!\\n\")\n"
      "\n");*/
}

int main(int argc, char *argv[])
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);


    /* Set defaults -- we have to use strdup so that
       fuse_opt_parse can free the defaults if other
       values are specified */
    options.filename = strdup("hello");
    options.contents = strdup("Hello World!\n");
    options.greeter = new GreeterClient(grpc::CreateChannel(
                "localhost:50051", grpc::InsecureChannelCredentials()));
    /* Parse options */
    if (fuse_opt_parse(&args, &options, option_spec, NULL) == -1)
        return 1;

    /* When --help is specified, first print our own file-system
       specific help text, then signal fuse_main to show
       additional help (by adding `--help` to the options again)
       without usage: line (by setting argv[0] to the empty
       string) */
    if (options.show_help) {
        show_help(argv[0]);
        assert(fuse_opt_add_arg(&args, "--help") == 0);
        args.argv[0] = (char*) "";
    }


    return fuse_main(args.argc, args.argv, &hello_oper_init, NULL);
}
