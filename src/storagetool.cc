/*
 Copyright (C) 2018 Fredrik Öhrström

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

#include "storagetool.h"

#include "backup.h"
#include "filesystem_helpers.h"
#include "log.h"
#include "system.h"
#include "storage_rclone.h"

#include <algorithm>

static ComponentId STORAGETOOL = registerLogComponent("storagetool");
static ComponentId RCLONE = registerLogComponent("rclone");
static ComponentId CACHE = registerLogComponent("cache");

using namespace std;

struct StorageToolImplementation : public StorageTool
{
    StorageToolImplementation(ptr<System> sys, ptr<FileSystem> local_fs);

    RC storeBackupIntoStorage(Backup *backup,
                              Storage *storage,
                              StoreStatistics *st,
                              Settings *settings);

    RC listPointsInTime(Storage *storage, vector<pair<Path*,struct timespec>> *v);

    RC listBeakFiles(Storage *storage,
                     std::vector<TarFileName> *files,
                     std::vector<TarFileName> *bad_files,
                     std::vector<std::string> *other_files,
                     map<Path*,FileStat> *contents);

    RC sendBeakFilesToStorage(Path *dir, Storage *storage, std::vector<TarFileName*> *files);
    RC fetchBeakFilesFromStorage(Storage *storage, std::vector<TarFileName*> *files, Path *dir);

    FileSystem *asCachedReadOnlyFS(Storage *storage);

    System *sys_;
    FileSystem *local_fs_;
};

unique_ptr<StorageTool> newStorageTool(ptr<System> sys,
                                       ptr<FileSystem> local_fs)
{
    return unique_ptr<StorageTool>(new StorageToolImplementation(sys, local_fs));
}

StorageToolImplementation::StorageToolImplementation(ptr<System>sys,
                                                     ptr<FileSystem> local_fs)
    : sys_(sys), local_fs_(local_fs)
{

}

void add_backup_work(ptr<StoreStatistics> st,
                     Path *path, FileStat *stat,
                     Settings *settings,
                     FileSystem *to_fs)
{
    Path *file_to_extract = path->prepend(settings->to.storage->storage_location);

    if (stat->isRegularFile()) {
        assert(st->stats.file_sizes.count(file_to_extract) == 0);
        st->stats.file_sizes[file_to_extract] = stat->st_size;
        stat->checkStat(to_fs, file_to_extract);
        if (stat->disk_update == Store) {
            st->stats.num_files_to_store++;
            st->stats.size_files_to_store += stat->st_size;
        }
        st->stats.num_files++;
        st->stats.size_files+=stat->st_size;
        //printf("PREP %ju/%ju \"%s\"\n", st->stats.num_files_to_store, st->stats.num_files, file_to_extract->c_str());
    }
    else if (stat->isDirectory()) st->stats.num_dirs++;
}

void store_local_backup_file(Backup *backup,
                             FileSystem *backup_fs,
                             FileSystem *origin_fs,
                             FileSystem *storage_fs,
                             Path *path,
                             FileStat *stat,
                             Settings *settings,
                             ptr<StoreStatistics> st)
{
    if (!stat->isRegularFile()) return;

    TarFile *tar = backup->findTarFromPath(path);
    assert(tar);

    debug(STORAGETOOL, "PATH %s\n", path->c_str());
    Path *file_name = path->prepend(settings->to.storage->storage_location);
    storage_fs->mkDirpWriteable(file_name->parent());
    FileStat old_stat;
    RC rc = storage_fs->stat(file_name, &old_stat);
    if (rc.isOk() &&
        stat->samePermissions(&old_stat) &&
        stat->sameSize(&old_stat) &&
        stat->sameMTime(&old_stat)) {

        debug(STORAGETOOL, "Skipping %s\n", file_name->c_str());
    } else {
        if (rc.isOk()) {
            storage_fs->deleteFile(file_name);
        }
        // The size gets incrementally update while the tar file is written!
        auto func = [&st](size_t n){ st->stats.size_files_stored += n; };
        tar->createFile(file_name, stat, origin_fs, storage_fs, 0, func);

        storage_fs->utime(file_name, stat);
        st->stats.num_files_stored++;
        verbose(STORAGETOOL, "Stored %s\n", file_name->c_str());
    }
//    st->num_files_handled++;
//    st->size_files_handled += stat->st_size;
    st->updateProgress();
}

RC StorageToolImplementation::storeBackupIntoStorage(Backup  *backup,
                                                     Storage *storage,
                                                     StoreStatistics *st,
                                                     Settings *settings)
{
    st->startDisplayOfProgress();

    // The backup archive files (.tar .gz) are found here.
    FileSystem *backup_fs = backup->asFileSystem();
    // The where the origin files can be found.
    FileSystem *origin_fs = backup->originFileSystem();
    // Store the archive files here.
    FileSystem *storage_fs = NULL;

    map<Path*,FileStat> contents;
    unique_ptr<FileSystem> fs;
    if (storage->type == FileSystemStorage) {
        storage_fs = local_fs_;
    } else
    if (storage->type == RCloneStorage) {
        vector<TarFileName> files, bad_files;
        vector<string> other_files;
        RC rc = listBeakFiles(storage, &files, &bad_files, &other_files, &contents);
        if (rc.isErr()) {
            error(STORAGETOOL, "Could not list files in rclone storage %s\n", storage->storage_location->c_str());
        }

        fs = newStatOnlyFileSystem(contents);
        storage_fs = fs.get();
    }
    backup_fs->recurse(Path::lookupRoot(), [=]
                       (Path *path, FileStat *stat) {
                           add_backup_work(st, path, stat, settings, storage_fs);
                       });

    debug(STORAGETOOL, "Work to be done: num_files=%ju num_dirs=%ju\n", st->stats.num_files, st->stats.num_dirs);

    switch (storage->type) {
    case FileSystemStorage:
    {
        backup_fs->recurse(Path::lookupRoot(), [=]
                           (Path *path, FileStat *stat) {store_local_backup_file(backup,
                                                                                 backup_fs,
                                                                                 origin_fs,
                                                                                 storage_fs,
                                                                                 path,
                                                                                 stat,
                                                                                 settings,
                                                                                 st); });
        break;
    }
    case RSyncStorage: break;
    case RCloneStorage:
    {
        Path *mount = local_fs_->mkTempDir("beak_push_");
        unique_ptr<FuseMount> fuse_mount = sys_->mount(mount, backup->asFuseAPI(), settings->fusedebug);

        if (!fuse_mount) {
            error(STORAGETOOL, "Could not mount beak filesyset for rclone.\n");
        }

        vector<string> args;
        args.push_back("copy");
        args.push_back("-v");
        args.push_back(mount->c_str());
        args.push_back(storage->storage_location->str());
        vector<char> output;
        RC rc = sys_->invoke("rclone", args, &output, CaptureBoth,
                             [&st, storage](char *buf, size_t len) {
                                 size_t from, to;
                                 for (from=1; from<len-1; ++from) {
                                     if (buf[from-1] == ' ' && buf[from] == ':' && buf[from+1] == ' ') {
                                         from = from+2;
                                         break;
                                     }
                                 }
                                 for (to=len-2; to>from; --to) {
                                     if (buf[to] == ':' && buf[to+1] == ' ') {
                                         break;
                                     }
                                 }
                                 string file = storage->storage_location->str()+"/"+string(buf+from, to-from);
                                 Path *path = Path::lookup(file);
                                 size_t size = 0;

                                 debug(RCLONE, "copied: %ju \"%s\"\n", st->stats.file_sizes.count(path), path->c_str());
                                 if (st->stats.file_sizes.count(path)) {
                                     size = st->stats.file_sizes[path];
                                     st->stats.size_files_stored += size;
                                     st->stats.num_files_stored++;
                                     st->updateProgress();
                                 }
                             });
        // Parse verbose output and look for:
        // 2018/01/29 20:05:36 INFO  : code/src/s01_001517180913.689221661_11659264_b6f526ca4e988180fe6289213a338ab5a4926f7189dfb9dddff5a30ab50fc7f3_0.tar: Copied (new)

        if (rc.isErr()) {
            error(STORAGETOOL, "Error when invoking rclone.\n");
        }

        // Unmount virtual filesystem.
        rc = sys_->umount(fuse_mount);
        if (rc.isErr()) {
            error(STORAGETOOL, "Could not unmount beak filesystem \"%s\".\n", mount->c_str());
        }
        rc = local_fs_->rmDir(mount);

        break;
    }
    case NoSuchStorage:
        assert(0);
    }

    st->finishProgress();

    return RC::OK;
}

RC StorageToolImplementation::sendBeakFilesToStorage(Path *dir, Storage *storage, vector<TarFileName*> *files)
{
    assert(storage->type == RCloneStorage);

    string files_to_fetch;
    Path *tmp;
    if (files) {
        for (auto& tfn : *files) {
            files_to_fetch.append(tfn->path->str());
            files_to_fetch.append("\n");
        }
    }

    tmp = local_fs_->mkTempFile("beak_fetching", files_to_fetch);

    RC rc = RC::OK;
    vector<char> out;
    vector<string> args;
    args.push_back("copy");
    if (files) {
        args.push_back("--include-from");
        args.push_back(tmp->c_str());
    }
    args.push_back(dir->c_str());
    args.push_back(storage->storage_location->c_str());
    rc = sys_->invoke("rclone", args, &out);

    return rc;
}

RC StorageToolImplementation::fetchBeakFilesFromStorage(Storage *storage, vector<TarFileName*> *files, Path *dir)
{
    assert(storage->type == RCloneStorage);

    string files_to_fetch;
    for (auto& tfn : *files) {
        files_to_fetch.append(tfn->path->str());
        files_to_fetch.append("\n");
    }

    Path *tmp = local_fs_->mkTempFile("beak_fetching", files_to_fetch);

    RC rc = RC::OK;
    vector<char> out;
    vector<string> args;
    args.push_back("copy");
    args.push_back("--include-from");
    args.push_back(tmp->c_str());
    args.push_back(storage->storage_location->c_str());
    args.push_back(dir->c_str());
    rc = sys_->invoke("rclone", args, &out);

    return rc;
}

RC StorageToolImplementation::listBeakFiles(Storage *storage,
                                            vector<TarFileName> *files,
                                            vector<TarFileName> *bad_files,
                                            vector<string> *other_files,
                                            map<Path*,FileStat> *contents)
{
    assert(storage->type == RCloneStorage);

    RC rc = RC::OK;
    vector<char> out;
    vector<string> args;
    args.push_back("ls");
    args.push_back(storage->storage_location->c_str());
    rc = sys_->invoke("rclone", args, &out);

    if (rc.isErr()) return RC::ERR;

    auto i = out.begin();
    bool eof = false, err = false;

    for (;;) {
	// Example line:
	// 12288 z01_001506595429.268937346_0_7eb62d8e0097d5eaa99f332536236e6ba9dbfeccf0df715ec96363f8ddd495b6_0.gz
        eatWhitespace(out, i, &eof);
        if (eof) break;
        string size = eatTo(out, i, ' ', 64, &eof, &err);

        if (eof || err) break;
        string file_name = eatTo(out, i, '\n', 4096, &eof, &err);
        if (err) break;
        TarFileName tfn;
        bool ok = TarFile::parseFileName(file_name, &tfn);
        // Only files that have proper beakfs names are included.
        if (ok) {
            // Check that the remote size equals the content. If there is a mismatch,
            // then for sure the file must be overwritte/updated. Perhaps there was an earlier
            // transfer interruption....
            size_t siz = (size_t)atol(size.c_str());
            if ( (tfn.type != REG_FILE && tfn.size == siz) ||
                 (tfn.type == REG_FILE && tfn.size == 0) )
            {
                files->push_back(tfn);
                Path *p = tfn.path->prepend(storage->storage_location);
                FileStat fs;
                fs.st_size = (off_t)siz;
                fs.st_mtim.tv_sec = tfn.secs;
                fs.st_mtim.tv_nsec = tfn.nsecs;
                (*contents)[p] = fs;
            }
            else
            {
                bad_files->push_back(tfn);
            }
        } else {
            other_files->push_back(file_name);
        }
    }
    if (err) return RC::ERR;

    return RC::OK;
}

RC StorageToolImplementation::listPointsInTime(Storage *storage, vector<pair<Path*,struct timespec>> *v)
{
    switch (storage->type) {
    case FileSystemStorage:
    {
        break;
    }
    case RSyncStorage:
    {
        /*
        vector<char> out;
        vector<string> args;
        args.push_back("--list-only");
        args.push_back(storage->storage_location->str());
        RC rc = sys_->invoke("rsync", args, &out);
        if (rc.isErr()) return RC::ERR;
        */
        /*auto i = out.begin();
        bool eof, err;

        for (;;) {
            // Example line:
            // -rw-rw-r--         15,920 2018/05/26 08:43:32 z01_.....gz
            }*/
        break;
    }
    case RCloneStorage:
    {
        vector<TarFileName> files;
        vector<TarFileName> bad_files;
        vector<std::string> other_files;
        map<Path*,FileStat> contents;
        RC rc = rcloneListBeakFiles(storage, &files, &bad_files, &other_files, &contents, sys_);
        if (rc.isErr()) return RC::ERR;

        /*
        sort(v->begin(), v->end(),
             [](struct timespec &a, struct timespec &b)->bool {
                 return (b.tv_sec < a.tv_sec) ||
                     (b.tv_sec == a.tv_sec &&
                      b.tv_nsec < a.tv_nsec);
             });
        */

        break;
    }
    case NoSuchStorage:
        assert(0);
    }

    return RC::OK;
}

struct CacheFS : ReadOnlyCacheFileSystemBaseImplementation
{
    CacheFS(ptr<FileSystem> cache_fs, Path *cache_dir, Storage *storage, System *sys) :
        ReadOnlyCacheFileSystemBaseImplementation("CacheFS", cache_fs, cache_dir),
        sys_(sys), storage_(storage) {}

    void refreshCache();
    RC loadDirectoryStructure(std::map<Path*,CacheEntry> *entries);
    RC fetchFile(Path *file);
    RC fetchFiles(vector<Path*> *files);

protected:

    System *sys_ {};
    Storage *storage_ {};
};

void CacheFS::refreshCache() {
    loadDirectoryStructure(&entries_);
}

RC CacheFS::loadDirectoryStructure(map<Path*,CacheEntry> *entries)
{
    vector<TarFileName> files;
    vector<TarFileName> bad_files;
    vector<std::string> other_files;
    map<Path*,FileStat> contents;

    switch (storage_->type) {
    case NoSuchStorage:
    case FileSystemStorage:
    {
        break;
    }
    case RSyncStorage:
    case RCloneStorage:
        fprintf(stdout, "Fetching list of files in %s ...", storage_->storage_location->c_str());
        fflush(stdout);
        RC rc = rcloneListBeakFiles(storage_, &files, &bad_files, &other_files, &contents, sys_);
        fprintf(stdout, "done. (%zu files)\n", files.size());
        fflush(stdout);
        if (rc.isErr()) return RC::ERR;
    }

    Path *prev_dir = NULL;
    CacheEntry *prev_dir_cache_entry = NULL;
    FileStat dir_stat;
    dir_stat.setAsDirectory();

    for (auto&p:contents) {
        Path *dir = p.first->parent();
        CacheEntry *dir_entry = NULL;
        if (dir == prev_dir) {
            dir_entry = prev_dir_cache_entry;
        } else {
            if (entries->count(dir) == 0) {
                (*entries)[dir] = CacheEntry(dir_stat, dir, true);
            }
            dir_entry = prev_dir_cache_entry = &(*entries)[dir];
        }
        (*entries)[p.first] = CacheEntry(p.second, p.first, false);
        // Add this file to its directory.
        dir_entry->direntries.push_back(&(*entries)[p.first]);
    }

    for (auto&p:*entries) {
        if (p.second.direntries.size()) {
//            fprintf(stderr, ">%s<=  %zd %zd %s\n", p.first->c_str(), p.second.direntries.size(), p.second.fs.st_size, p.second.path->c_str());
        }
    }

    return RC::ERR;
}

RC CacheFS::fetchFile(Path *file)
{
    vector<Path*> files;
    files.push_back(file);
    return fetchFiles(&files);
}

RC CacheFS::fetchFiles(vector<Path*> *files)
{
    switch (storage_->type) {
    case NoSuchStorage:
    case FileSystemStorage:
    case RSyncStorage:
    case RCloneStorage:
    {
        debug(CACHE,"Fetching %d files from %s.\n", files->size(), storage_->storage_location->c_str());
        return rcloneFetchFiles(storage_, files, cache_dir_, sys_, cache_fs_);
    }
    }
    return RC::ERR;
}

FileSystem *StorageToolImplementation::asCachedReadOnlyFS(Storage *storage)
{
    Path *cache_dir = cacheDir();
    local_fs_->mkDirpWriteable(cache_dir);
    CacheFS *fs = new CacheFS(local_fs_, cache_dir, storage, sys_);
    fs->refreshCache();
    return fs;
}
