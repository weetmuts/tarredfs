/*
 Copyright (C) 2017 Fredrik Öhrström

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "system.h"

#include "filesystem.h"
#include "log.h"

#include <memory.h>
#include <pthread.h>
#include <sys/types.h>
#include <wait.h>

#include <unistd.h>

using namespace std;

static ComponentId SYSTEM = registerLogComponent("system");
static ComponentId SYSTEMIO = registerLogComponent("systemio");

struct ThreadCallbackImplementation : ThreadCallback
{
    ThreadCallbackImplementation(int millis, function<bool()> thread_cb);
    void stop();
    void doWhileCallbackBlocked(std::function<void()> do_cb);
    ~ThreadCallbackImplementation();

private:

    pthread_mutex_t execute_ {};
    bool running_ {};
    int millis_ {};
    function<bool()> regular_cb_;
    pthread_t thread_ {};

    friend void *regularThread(void *data);
};

void ThreadCallbackImplementation::stop()
{
    running_ = false;
    if (thread_) {
        //pthread_kill(thread_, SIGUSR2);
    }
}

void ThreadCallbackImplementation::doWhileCallbackBlocked(function<void()> do_cb)
{
    pthread_mutex_lock(&execute_);
    do_cb();
    pthread_mutex_unlock(&execute_);
}

void *regularThread(void *data)
{
    auto tcbi = (ThreadCallbackImplementation*)(data);

    time_t prev = time(NULL);
    time_t curr = prev;
    while (tcbi->running_) {
        curr = time(NULL);
        time_t diff = curr-prev;
        if (diff >= 1) {
            tcbi->doWhileCallbackBlocked(tcbi->regular_cb_);
            prev = curr;
        } else {
            int rc = usleep(1000000);
            if (rc == -1) {
                if (errno == EINTR) {
                    debug(SYSTEM, "regular thread callback awaken by signal.\n");
                } else {
                    error(SYSTEM, "could not sleep.\n");
                }
            }
        }
    }
    return NULL;
}

ThreadCallbackImplementation::ThreadCallbackImplementation(int millis, function<bool()> regular_cb)
    : millis_(millis), regular_cb_(regular_cb)
{
    running_ = true;
    int rc  = pthread_create(&thread_, NULL, regularThread, this);
    if (rc) {
        error(SYSTEM, "Could not create thread.\n");
    }
    millis_++;
}


ThreadCallbackImplementation::~ThreadCallbackImplementation()
{
    fprintf(stderr, "Destructing regular thread\n");
    if (thread_) {
        //pthread_kill(thread_, SIGUSR2);
        pthread_join(thread_, NULL);
        fprintf(stderr, "JOINED thread properly!\n");
    }
}

unique_ptr<ThreadCallback> newRegularThreadCallback(int millis, std::function<bool()> thread_cb)
{
    return unique_ptr<ThreadCallback>(new ThreadCallbackImplementation(millis, thread_cb));
}

function<void()> exit_handler_;

void exitHandler(int signum)
{
    if (exit_handler_) exit_handler_();
}

function<void()> child_exit_handler_;

void childExitHandler(int signum)
{
    if (child_exit_handler_) child_exit_handler_();
}

void doNothing(int signum) {
}

void onExit(string msg, function<void()> cb)
{
    exit_handler_ = cb;
    struct sigaction new_action, old_action;

    new_action.sa_handler = exitHandler;
    sigemptyset (&new_action.sa_mask);
    new_action.sa_flags = 0;

    sigaction (SIGINT, NULL, &old_action);
    if (old_action.sa_handler != SIG_IGN) sigaction(SIGINT, &new_action, NULL);

    sigaction (SIGHUP, NULL, &old_action);
    if (old_action.sa_handler != SIG_IGN) sigaction (SIGHUP, &new_action, NULL);

    sigaction (SIGTERM, NULL, &old_action);
    if (old_action.sa_handler != SIG_IGN) sigaction (SIGTERM, &new_action, NULL);
}

void onChildExit(string msg, function<void()> cb)
{
    child_exit_handler_ = cb;
    struct sigaction new_action, old_action;

    new_action.sa_handler = childExitHandler;
    sigemptyset (&new_action.sa_mask);
    new_action.sa_flags = 0;

    sigaction (SIGCHLD, NULL, &old_action);
    if (old_action.sa_handler != SIG_IGN) sigaction(SIGCHLD, &new_action, NULL);
}

struct SystemImplementation : System
{
    RC invoke(string program,
               vector<string> args,
              std::vector<char> *output = NULL,
              Capture capture = CaptureStdout,
              std::function<void(char *buf, size_t len)> output_cb = NULL);

    RC invokeShell(Path *init_file);

    RC mountDaemon(Path *dir, FuseAPI *fuseapi, bool foreground, bool debug);
    RC umountDaemon(Path *dir);
    std::unique_ptr<FuseMount> mount(Path *dir, FuseAPI *fuseapi, bool debug);
    RC umount(ptr<FuseMount> fuse_mount);

    RC mountInternal(Path *dir, FuseAPI *fuseapi,
                     bool daemon, unique_ptr<FuseMount> &fm,
                     bool foreground, bool debug);

    ~SystemImplementation() = default;

    private:

    pid_t running_shell_pid_ {};

};

unique_ptr<System> newSystem()
{
    return unique_ptr<System>(new SystemImplementation());
}

string protect_(string arg)
{
    return arg;
}

RC SystemImplementation::invoke(string program,
                                 vector<string> args,
                                 vector<char> *output,
                                 Capture capture,
                                 function<void(char *buf, size_t len)> cb)
{
    int link[2];
    const char **argv = new const char*[args.size()+2];
    argv[0] = program.c_str();
    int i = 1;
    debug(SYSTEM, "exec \"%s\"\n", program.c_str());
    for (auto &a : args) {
        argv[i] = a.c_str();
        i++;
        debug(SYSTEM, "arg \"%s\"\n", a.c_str());
    }
    argv[i] = NULL;

    if (output) {
        if (pipe(link) == -1) {
            error(SYSTEM, "Could not create pipe!\n");
        }
    }
    pid_t pid = fork();
    int status;
    if (pid == 0) {
        // I am the child!
        if (output) {
            if (capture == CaptureBoth || capture == CaptureStdout) {
                dup2 (link[1], STDOUT_FILENO);
            }
            if (capture == CaptureBoth || capture == CaptureStderr) {
                dup2 (link[1], STDERR_FILENO);
            }
            close(link[0]);
            close(link[1]);
        }
        close(0); // Close stdin
        execvp(program.c_str(), (char*const*)argv);
        perror("Execvp failed:");
        error(SYSTEM, "Invoking %s failed!\n", program.c_str());
    } else {
        if (pid == -1) {
            error(SYSTEM, "Could not fork!\n");
        }

        if (output) {
            close(link[1]);

            char buf[4096 + 1];

            int n = 0;

            for (;;) {
                memset(buf, 0, sizeof(buf));
                n = read(link[0], buf, sizeof(buf));
                if (n > 0) {
                    output->insert(output->end(), buf, buf+n);
                    if (cb) { cb(buf, n); }
                    debug(SYSTEMIO, "%s: \"%*s\"\n", program.c_str(), n, buf);
                } else {
                    // No more data to read.
                    debug(SYSTEMIO, "%s: done\n", program.c_str());
                    break;
                }
            }
        }
        debug(SYSTEM,"waiting for child %d.\n", pid);
        // Wait for the child to finish!
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            // Child exited properly.
            int rc = WEXITSTATUS(status);
            debug(SYSTEM,"%s: return code %d\n", program.c_str(), rc);
            if (rc != 0) {
                warning(SYSTEM,"%s exited with non-zero return code: %d\n", program.c_str(), rc);
                return RC::ERR;
            }
        }
    }
    return RC::OK;
}

RC SystemImplementation::invokeShell(Path *init_file)
{
    const char **argv = new const char*[4];
    argv[0] = "/bin/bash";
    argv[1] = "--init-file";
    argv[2] = init_file->c_str();
    argv[3] = NULL;
    debug(SYSTEM, "invoking shell: \"%s --init-file %s\"\n", argv[0], argv[2]);

    int pid = fork();

    if (pid == 0) {
        onExit("Invoke shell", [&](){
                // Cleanup here?
            });
        // This is the child process, run the shell here.
        execvp(argv[0], (char*const*)argv);
    } else {
        running_shell_pid_ = pid;
    }
    int status;
    waitpid(running_shell_pid_, &status, 0);
    logSystem(SYSTEM, "Beak shell exited!\n");

    return RC::OK;
}

RC SystemImplementation::mountDaemon(Path *dir, FuseAPI *fuseapi, bool foreground, bool debug)
{
    unique_ptr<FuseMount> fm;
    return mountInternal(dir, fuseapi, true, fm, foreground, debug);
}

RC SystemImplementation::umountDaemon(Path *dir)
{
    vector<char> out;
    vector<string> args;
    args.push_back("-u");
    args.push_back(dir->c_str());
    return invoke("fusermount", args, &out);
}

unique_ptr<FuseMount> SystemImplementation::mount(Path *dir, FuseAPI *fuseapi, bool debug)
{
    unique_ptr<FuseMount> fm;
    mountInternal(dir, fuseapi, false, fm, false, debug);
    return fm;
}

static int staticGetattrDispatch_(const char *path, struct stat *stbuf)
{
    FuseAPI *fuseapi = (FuseAPI*)fuse_get_context()->private_data;
    return fuseapi->getattrCB(path, stbuf);
}

static int staticReaddirDispatch_(const char *path, void *buf, fuse_fill_dir_t filler,
                                  off_t offset, struct fuse_file_info *fi)
{
    FuseAPI *fuseapi = (FuseAPI*)fuse_get_context()->private_data;
    return fuseapi->readdirCB(path, buf, filler, offset, fi);
}

static int staticReadDispatch_(const char *path, char *buf, size_t size, off_t offset,
                       struct fuse_file_info *fi)
{
    FuseAPI *fuseapi = (FuseAPI*)fuse_get_context()->private_data;
    return fuseapi->readCB(path, buf, size, offset, fi);
}

static int staticReadlinkDispatch_(const char *path, char *buf, size_t size)
{
    FuseAPI *fuseapi = (FuseAPI*)fuse_get_context()->private_data;
    return fuseapi->readlinkCB(path, buf, size);
}

static int staticOpenDispatch_(const char *path, struct fuse_file_info *fi)
{
    return 0;
}

struct FuseMountImplementationPosix : FuseMount
{
    Path *dir;
    struct fuse_chan *chan;
    struct fuse *fuse;
    pid_t loop_pid;
};

RC SystemImplementation::mountInternal(Path *dir, FuseAPI *fuseapi,
                                       bool daemon, unique_ptr<FuseMount> &fm,
                                       bool foreground, bool debug)
{
    vector<string> fuse_args;
    fuse_args.push_back("beak");
    if (foreground) fuse_args.push_back("-f");
    if (debug) fuse_args.push_back("-d");
    if (daemon) fuse_args.push_back(dir->str());

    int fuse_argc = fuse_args.size();
    char **fuse_argv = new char*[fuse_argc+1];
    int j = 0;
    for (auto &s : fuse_args) {
        fuse_argv[j] = (char*)s.c_str();
        j++;
    }
    fuse_argv[j] = 0;

    fuse_operations *ops = new fuse_operations;
    memset(ops, 0, sizeof(*ops));
    ops->getattr = staticGetattrDispatch_;
    ops->open = staticOpenDispatch_;
    ops->read = staticReadDispatch_;
    ops->readdir = staticReaddirDispatch_;
    ops->readlink = staticReadlinkDispatch_;

    if (daemon) {
        // The fuse daemon gracefully handles its own exit.
        // No need to track it here.
        int rc = fuse_main(fuse_argc, fuse_argv, ops, fuseapi);
        if (rc) return RC::ERR;
        return RC::OK;
    }

    auto *fuse_mount_info = new FuseMountImplementationPosix;

    struct fuse_args args;
    args.argc = fuse_argc;
    args.argv = fuse_argv;
    args.allocated = 0;
    fuse_mount_info->dir = dir;
    fuse_mount_info->chan = fuse_mount(dir->c_str(), &args);
    fuse_mount_info->fuse = fuse_new(fuse_mount_info->chan,
                                &args,
                                ops,
                                sizeof(*ops),
                                fuseapi);

    fuse_mount_info->loop_pid = fork();

    if (fuse_mount_info->loop_pid == 0) {
        // This is the child process.
        onExit("Child running fuse mount",
               [this,fuse_mount_info,dir](){
                logSystem(SYSTEM, "Child running fuse mount terminated! %s\n", dir->c_str());
                umount(fuse_mount_info);
                exit(-1);
            });
        // Serve the virtual file system.
        fuse_loop_mt (fuse_mount_info->fuse);
        exit(0);
    }
    onChildExit("Fuse mount",
                [this,fuse_mount_info,dir](){
                    pid_t pid;
                    int   status;
                    pid = waitpid(-1, &status, WNOHANG);
                    logSystem(SYSTEM, "Child %d %x exited! %s\n", pid, status, dir->c_str());
                });
    onExit("Beak process",
        [this,fuse_mount_info,dir](){
            logSystem(SYSTEM, "Beak terminated! \n", dir->c_str());
            umount(fuse_mount_info);
            if (running_shell_pid_ != 0) {
                kill(running_shell_pid_, SIGTERM);
            }
        });
    fm = unique_ptr<FuseMount>(fuse_mount_info);

    return RC::OK;
}

RC SystemImplementation::umount(ptr<FuseMount> fuse_mount_info)
{
    FuseMount *fm = fuse_mount_info;
    FuseMountImplementationPosix *fmi = (FuseMountImplementationPosix*)fm;
    fuse_exit(fmi->fuse);
    fuse_unmount (fmi->dir->c_str(), fmi->chan);
    return RC::OK;
}
