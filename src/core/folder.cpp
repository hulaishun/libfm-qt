/*
 *      fm-folder.c
 *
 *      Copyright 2009 - 2012 Hong Jen Yee (PCMan) <pcman.tw@gmail.com>
 *      Copyright 2012-2016 Andriy Grytsenko (LStranger) <andrej@rep.kiev.ua>
 *
 *      This file is a part of the Libfm library.
 *
 *      This library is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU Lesser General Public
 *      License as published by the Free Software Foundation; either
 *      version 2.1 of the License, or (at your option) any later version.
 *
 *      This library is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *      Lesser General Public License for more details.
 *
 *      You should have received a copy of the GNU Lesser General Public
 *      License along with this library; if not, write to the Free Software
 *      Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "folder.h"
#include <string.h>
#include <QTimer>
#include <QThreadPool>

#include "dirlistjob.h"

namespace Fm2 {

std::unordered_map<FilePath, std::weak_ptr<Folder>, FilePathHash> Folder::cache_;
std::mutex Folder::mutex_;

Folder::Folder():
    mon_changed{this, &Folder::onFileChangeEvents},
    dirlist_job{nullptr},
    /* for file monitor */
    has_idle_handler{false},
    pending_change_notify{false},
    filesystem_info_pending{false},
    wants_incremental{false},
    idle_reload_handler{0},
    stop_emission{false}, /* don't set it 1 bit to not lock other bits */
    /* filesystem info - set in query thread, read in main */
    fs_total_size{0},
    fs_free_size{0},
    has_fs_info{false},
    fs_info_not_avail{false},
    defer_content_test{false} {
}

Folder::Folder(const FilePath& path): Folder() {
    dir_path = path;
}

Folder::~Folder() {
    // We store a weak_ptr instead of shared_ptr in the hash table, so the hash table
    // does not own a reference to the folder. When the last reference to Folder is
    // freed, we need to remove its hash table entry.
    std::lock_guard<std::mutex> lock{mutex_};
    auto it = cache_.find(dir_path);
    if(it != cache_.end()) {
        cache_.erase(it);
    }
}

// static
std::shared_ptr<Folder> Folder::fromPath(const FilePath& path) {
    std::lock_guard<std::mutex> lock{mutex_};
    auto it = cache_.find(path);
    if(it != cache_.end()) {
        auto folder = it->second.lock();
        if(folder) {
            return folder;
        }
        else { // FIXME: is this possible?
            cache_.erase(it);
        }
    }
    auto folder = std::make_shared<Folder>(path);
    cache_.insert(std::make_pair(path, folder));
    return folder;
}

bool Folder::makeDirectory(const char* name, GError** error) {

}

void Folder::queryFilesystemInfo() {

}

bool Folder::getFilesystemInfo(uint64_t* total_size, uint64_t* free_size) const {

}

bool Folder::isIncremental() const {
    return wants_incremental;
}

bool Folder::isValid() const {
    return dir_fi != nullptr;
}

bool Folder::isLoaded() const {

}

std::shared_ptr<const FileInfo> Folder::getFileByName(const char* name) const {
    auto it = files.find(name);
    if(it != files.end()) {
        return it->second;
    }
    return nullptr;
}

bool Folder::isEmpty() const {
    return files.empty();
}

FileInfoList Folder::getFiles() const {
    FileInfoList ret;
    ret.reserve(files.size());
    for(const auto& item : files) {
        ret.push_back(item.second);
    }
    return ret;
}


const FilePath& Folder::getPath() const {
    return dir_path;
}

const std::shared_ptr<const FileInfo>& Folder::getInfo() const {
    return dir_fi;
}

void Folder::unblockUpdates() {

}

void Folder::blockUpdates() {

}


#if 0
void Folder::init(FmFolder* folder) {
    files = fm_file_info_list_new();
    G_LOCK(hash);
    if(G_UNLIKELY(hash_uses == 0)) {
        hash = g_hash_table_new((GHashFunc)fm_path_hash, (GEqualFunc)fm_path_equal);
        volume_monitor = g_volume_monitor_get();
        if(G_LIKELY(volume_monitor)) {
            g_signal_connect(volume_monitor, "mount-added", G_CALLBACK(on_mount_added), NULL);
            g_signal_connect(volume_monitor, "mount-removed", G_CALLBACK(on_mount_removed), NULL);
        }
    }
    hash_uses++;
    G_UNLOCK(hash);
}

bool on_idle_reload(FmFolder* folder) {
    /* check if folder still exists */
    if(g_source_is_destroyed(g_main_current_source())) {
        /* FIXME: it should be impossible, folder cannot be disposed at this point */
        return false;
    }
    Folder::reload(folder);
    G_LOCK(query);
    idle_reload_handler = 0;
    G_UNLOCK(query);
    g_object_unref(folder);
    return false;
}

void queue_reload(FmFolder* folder) {
    G_LOCK(query);
    if(!idle_reload_handler)
        idle_reload_handler = g_idle_add_full(G_PRIORITY_LOW, (GSourceFunc)on_idle_reload,
                                      g_object_ref(folder), NULL);
    G_UNLOCK(query);
}

void on_file_info_job_finished(FmFileInfoJob* job, FmFolder* folder) {
    GList* l;
    GSList* files_to_add = NULL;
    GSList* files_to_update = NULL;
    if(!fm_job_is_cancelled(FM_JOB(job))) {
        bool need_added = g_signal_has_handler_pending(folder, signals[FILES_ADDED], 0, true);
        bool need_changed = g_signal_has_handler_pending(folder, signals[FILES_CHANGED], 0, true);

        for(l = fm_file_info_list_peek_head_link(job->file_infos); l; l = l->next) {
            FmFileInfo* fi = (FmFileInfo*)l->data;
            FmPath* path = fm_file_info_get_path(fi);
            GList* l2;
            if(path == fm_file_info_get_path(dir_fi))
                /* update for folder itself, also see FIXME below! */
            {
                fm_file_info_update(dir_fi, fi);
            }
            else if((l2 = _Folder::get_file_by_path(folder, path)))
                /* the file is already in the folder, update */
            {
                FmFileInfo* fi2 = (FmFileInfo*)l2->data;
                /* FIXME: will fm_file_info_update here cause problems?
                 *        the file info might be referenced by others, too.
                 *        we're mofifying an object referenced by others.
                 *        we should redesign the API, or document this clearly
                 *        in future API doc.
                 */
                fm_file_info_update(fi2, fi);
                if(need_changed) {
                    files_to_update = g_slist_prepend(files_to_update, fi2);
                }
            }
            else {
                if(need_added) {
                    files_to_add = g_slist_prepend(files_to_add, fi);
                }
                fm_file_info_list_push_tail(files, fi);
            }
        }
        if(files_to_add) {
            g_signal_emit(folder, signals[FILES_ADDED], 0, files_to_add);
            g_slist_free(files_to_add);
        }
        if(files_to_update) {
            g_signal_emit(folder, signals[FILES_CHANGED], 0, files_to_update);
            g_slist_free(files_to_update);
        }
        g_signal_emit(folder, signals[CONTENT_CHANGED], 0);
    }
    pending_jobs = g_slist_remove(pending_jobs, job);
    g_object_unref(job);
}

#endif

void Folder::processPendingChanges() {
    has_idle_handler = false;
    // FmFileInfoJob* job = NULL;
    std::lock_guard<std::mutex> lock{mutex_};

    // idle_handler = 0;
    /* if we were asked to block updates let delay it for now */
    if(stop_emission) {
        return;
    }

#if 0
    if(files_to_update || files_to_add) {
        job = (FmFileInfoJob*)fm_file_info_job_new(NULL, 0);
    }

    if(files_to_update) {
        for(l = files_to_update; l; l = l->next) {
            FmPath* path = l->data;
            fm_file_info_job_add(job, path);
            fm_path_unref(path);
        }
        g_slist_free(files_to_update);
    }

    if(files_to_add) {
        for(l = files_to_add; l; l = l->next) {
            FmPath* path = l->data;
            fm_file_info_job_add(job, path);
            fm_path_unref(path);
        }
        g_slist_free(files_to_add);
    }

    if(job) {
        g_signal_connect(job, "finished", G_CALLBACK(on_file_info_job_finished), folder);
        pending_jobs = g_slist_prepend(pending_jobs, job);
        if(!fm_job_run_async(FM_JOB(job))) {
            pending_jobs = g_slist_remove(pending_jobs, job);
            g_object_unref(job);
            g_critical("failed to start folder update job");
        }
        /* the job will be freed automatically in on_file_info_job_finished() */
    }
#endif
    if(!files_to_del.empty()) {
        FileInfoList deleted_files;
        for(auto path: files_to_del) {
            auto name = path.baseName();
            auto it = files.find(name.get());
            if(it != files.end()) {
                deleted_files.push_back(it->second);
                files.erase(it);
            }
        }
        Q_EMIT filesRemoved(deleted_files);
        Q_EMIT contentChanged();
    }

    if(pending_change_notify) {
        Q_EMIT changed();
        /* update volume info */
        queryFilesystemInfo();
        pending_change_notify = false;
    }

    if(filesystem_info_pending) {
        Q_EMIT fileSystemChanged();
        filesystem_info_pending = false;
    }
}

/* should be called only with G_LOCK(lists) on! */
void Folder::queue_update() {
    if(!has_idle_handler) {
        QTimer::singleShot(0, this, &Folder::processPendingChanges);
        has_idle_handler = true;
    }
}


#if 0

/* returns true if reference was taken from path */
bool _Folder::event_file_added(FmFolder* folder, FmPath* path) {
    bool added = true;

    G_LOCK(lists);
    /* make sure that the file is not already queued for addition. */
    if(!g_slist_find(files_to_add, path)) {
        GList* l = _Folder::get_file_by_path(folder, path);
        if(!l) { /* it's new file */
            /* add the file name to queue for addition. */
            files_to_add = g_slist_append(files_to_add, path);
        }
        else if(g_slist_find(files_to_update, path)) {
            /* file already queued for update, don't duplicate */
            added = false;
        }
        /* if we already have the file in FmFolder, update the existing one instead. */
        else {
            /* bug #3591771: 'ln -fns . test' leave no file visible in folder.
               If it is queued for deletion then cancel that operation */
            files_to_del = g_slist_remove(files_to_del, l);
            /* update the existing item. */
            files_to_update = g_slist_append(files_to_update, path);
        }
    }
    else
        /* file already queued for adding, don't duplicate */
    {
        added = false;
    }
    if(added) {
        queue_update(folder);
    }
    G_UNLOCK(lists);
    return added;
}

bool _Folder::event_file_changed(FmFolder* folder, FmPath* path) {
    bool added;

    G_LOCK(lists);
    /* make sure that the file is not already queued for changes or
     * it's already queued for addition. */
    if(!g_slist_find(files_to_update, path) &&
            !g_slist_find(files_to_add, path) &&
            _Folder::get_file_by_path(folder, path)) { /* ensure it is our file */
        files_to_update = g_slist_append(files_to_update, path);
        added = true;
        queue_update(folder);
    }
    else {
        added = false;
    }
    G_UNLOCK(lists);
    return added;
}

void _Folder::event_file_deleted(FmFolder* folder, FmPath* path) {
    GList* l;
    GSList* sl;

    G_LOCK(lists);
    l = _Folder::get_file_by_path(folder, path);
    if(l && !g_slist_find(files_to_del, l)) {
        files_to_del = g_slist_prepend(files_to_del, l);
    }
    /* if the file is already queued for addition or update, that operation
       will be just a waste, therefore cancel it right now */
    sl = g_slist_find(files_to_update, path);
    if(sl) {
        files_to_update = g_slist_delete_link(files_to_update, sl);
    }
    else if((sl = g_slist_find(files_to_add, path))) {
        files_to_add = g_slist_delete_link(files_to_add, sl);
    }
    else {
        path = NULL;
    }
    queue_update(folder);
    G_UNLOCK(lists);
    if(path != NULL) {
        fm_path_unref(path);    /* link was freed above so we should unref it */
    }
}

#endif

void Folder::onDirChanged(GFileMonitorEvent evt) {
    switch(evt) {
    case G_FILE_MONITOR_EVENT_PRE_UNMOUNT:
        /* g_debug("folder is going to be unmounted"); */
        break;
    case G_FILE_MONITOR_EVENT_UNMOUNTED:
        Q_EMIT unmount();
        /* g_debug("folder is unmounted"); */
        // queue_reload(folder);
        break;
    case G_FILE_MONITOR_EVENT_DELETED:
        Q_EMIT removed();
        /* g_debug("folder is deleted"); */
        break;
    case G_FILE_MONITOR_EVENT_CREATED:
        // queue_reload(folder);
        break;
    case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
    case G_FILE_MONITOR_EVENT_CHANGED: {
        std::lock_guard<std::mutex> lock{mutex_};
        pending_change_notify = true;
        if(std::find(files_to_update.cbegin(), files_to_update.cend(), dir_path) != files_to_update.end()) {
            files_to_update.push_back(dir_path);
            queue_update();
        }
        /* g_debug("folder is changed"); */
        break;
    }
#if GLIB_CHECK_VERSION(2,24,0)
    case G_FILE_MONITOR_EVENT_MOVED:
#endif
    case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
        ;
    default:
        break;
    }
}

void Folder::onFileChangeEvents(GFileMonitor* monitor, GFile* gf, GFile* other_file, GFileMonitorEvent evt) {
    /* const char* names[]={
        "G_FILE_MONITOR_EVENT_CHANGED",
        "G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT",
        "G_FILE_MONITOR_EVENT_DELETED",
        "G_FILE_MONITOR_EVENT_CREATED",
        "G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED",
        "G_FILE_MONITOR_EVENT_PRE_UNMOUNT",
        "G_FILE_MONITOR_EVENT_UNMOUNTED"
    }; */
    if(dir_path.gfile() == gf) {
        onDirChanged(evt);
        return;
    }
    else {
        std::lock_guard<std::mutex> lock{mutex_};
        auto path = FilePath{gf, true};
        /* NOTE: sometimes, for unknown reasons, GFileMonitor gives us the
         * same event of the same file for multiple times. So we need to
         * check for duplications ourselves here. */
        switch(evt) {
        case G_FILE_MONITOR_EVENT_CREATED:
            if(std::find(files_to_add.cbegin(), files_to_add.cend(), path) != files_to_add.end()) {
                if(std::find(files_to_update.cbegin(), files_to_update.cend(), path) != files_to_update.end()) {
                    files_to_add.push_back(path);
                }
            }
            break;
        case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
        case G_FILE_MONITOR_EVENT_CHANGED:
            if(std::find(files_to_add.cbegin(), files_to_add.cend(), path) != files_to_add.end()) {
                if(std::find(files_to_update.cbegin(), files_to_update.cend(), path) != files_to_update.end()) {
                    files_to_update.push_back(path);
                }
            }
            break;
        case G_FILE_MONITOR_EVENT_DELETED:
            if(std::find(files_to_del.cbegin(), files_to_del.cend(), path) != files_to_del.end()) {
                files_to_del.push_back(path);
            }
            break;
        default:
            return;
        }
        queue_update();
    }
}

void Folder::onDirListFinished() {
    DirListJob* job = static_cast<DirListJob*>(sender());
    if(job->isCancelled() || job != dirlist_job) // this is a cancelled job, ignore!
        return;
#if 0
    if(dirlist_job->isCancelled() && !wants_incremental) {
        GList* l;
        for(l = fm_file_info_list_peek_head_link(job->files); l; l = l->next) {
            FmFileInfo* inf = (FmFileInfo*)l->data;
            files = g_slist_prepend(files, inf);
            fm_file_info_list_push_tail(files, inf);
        }
        if(G_LIKELY(files)) {
            GSList* l;

            G_LOCK(lists);
            if(defer_content_test && fm_path_is_native(dir_path))
                /* we got only basic info on content, schedule update it now */
                for(l = files; l; l = l->next)
                    files_to_update = g_slist_prepend(files_to_update,
                                              fm_path_ref(fm_file_info_get_path(l->data)));
            G_UNLOCK(lists);
            g_signal_emit(folder, signals[FILES_ADDED], 0, files);
            g_slist_free(files);
        }

        if(job->dir_fi) {
            dir_fi = fm_file_info_ref(job->dir_fi);
        }

        /* Some new files are created while FmDirListJob is loading the folder. */
        G_LOCK(lists);
        if(G_UNLIKELY(files_to_add)) {
            /* This should be a very rare case. Could this happen? */
            GSList* l;
            for(l = files_to_add; l;) {
                FmPath* path = l->data;
                GSList* next = l->next;
                if(_Folder::get_file_by_path(folder, path)) {
                    /* we already have the file. remove it from files_to_add,
                     * and put it in files_to_update instead.
                     * No strdup for name is needed here. We steal
                     * the string from files_to_add.*/
                    files_to_update = g_slist_prepend(files_to_update, path);
                    files_to_add = g_slist_delete_link(files_to_add, l);
                }
                l = next;
            }
        }
        G_UNLOCK(lists);
    }
    else if(!dir_fi && job->dir_fi)
        /* we may need dir_fi for incremental folders too */
    {
        dir_fi = fm_file_info_ref(job->dir_fi);
    }
    g_object_unref(dirlist_job);
    dirlist_job = NULL;

#endif
    Q_EMIT finishLoading();
}


#if 0


void on_dirlist_job_files_found(FmDirListJob* job, GSList* files, gpointer user_data) {
    FmFolder* folder = FM_FOLDER(user_data);
    GSList* l;
    for(l = files; l; l = l->next) {
        FmFileInfo* file = FM_FILE_INFO(l->data);
        fm_file_info_list_push_tail(files, file);
    }
    if(G_UNLIKELY(!dir_fi && job->dir_fi))
        /* we may want info while folder is still loading */
    {
        dir_fi = fm_file_info_ref(job->dir_fi);
    }
    g_signal_emit(folder, signals[FILES_ADDED], 0, files);
}

FmJobErrorAction on_dirlist_job_error(FmDirListJob* job, GError* err, FmJobErrorSeverity severity, FmFolder* folder) {
    guint ret;
    /* it's possible that some signal handlers tries to free the folder
     * when errors occurs, so let's g_object_ref here. */
    g_object_ref(folder);
    g_signal_emit(folder, signals[ERROR], 0, err, (guint)severity, &ret);
    g_object_unref(folder);
    return ret;
}

FmFolder* Folder::new_internal(FmPath* path, GFile* gf) {
    FmFolder* folder = (FmFolder*)g_object_new(FM_TYPE_FOLDER, NULL);
    dir_path = fm_path_ref(path);
    gf = (GFile*)g_object_ref(gf);
    wants_incremental = fm_file_wants_incremental(gf);
    Folder::reload(folder);
    return folder;
}

void free_dirlist_job(FmFolder* folder) {
    if(wants_incremental) {
        g_signal_handlers_disconnect_by_func(dirlist_job, on_dirlist_job_files_found, folder);
    }
    g_signal_handlers_disconnect_by_func(dirlist_job, on_dirlist_job_finished, folder);
    g_signal_handlers_disconnect_by_func(dirlist_job, on_dirlist_job_error, folder);
    fm_job_cancel(FM_JOB(dirlist_job));
    g_object_unref(dirlist_job);
    dirlist_job = NULL;
}

void Folder::finalize(GObject* object) {
    G_LOCK(hash);
    hash_uses--;
    if(G_UNLIKELY(hash_uses == 0)) {
        g_hash_table_destroy(hash);
        hash = NULL;
        if(volume_monitor) {
            g_signal_handlers_disconnect_by_func(volume_monitor, on_mount_added, NULL);
            g_signal_handlers_disconnect_by_func(volume_monitor, on_mount_removed, NULL);
            g_object_unref(volume_monitor);
            volume_monitor = NULL;
        }
    }
    G_UNLOCK(hash);

    (* G_OBJECT_CLASS(Folder::parent_class)->finalize)(object);
}


#endif


void Folder::reload() {
    GError* err = NULL;

    /* Tell the world that we're about to reload the folder.
     * It might be a good idea for users of the folder to disconnect
     * from the folder temporarily and reconnect to it again after
     * the folder complete the loading. This might reduce some
     * unnecessary signal handling and UI updates. */

    Q_EMIT startLoading();
    if(dir_fi) {
        dir_fi = nullptr;
    }

    /* clear all update-lists now, see SF bug #919 - if update comes before
       listing job is finished, a duplicate may be created in the folder */
#if 0
    if(has_idle_handler) {
        g_source_remove(idle_handler);
        idle_handler = 0;
        if(files_to_add) {
            g_slist_foreach(files_to_add, (GFunc)fm_path_unref, NULL);
            g_slist_free(files_to_add);
            files_to_add = NULL;
        }
        if(files_to_update) {
            g_slist_foreach(files_to_update, (GFunc)fm_path_unref, NULL);
            g_slist_free(files_to_update);
            files_to_update = NULL;
        }
        if(files_to_del) {
            g_slist_free(files_to_del);
            files_to_del = NULL;
        }
    }
    /* remove all items and re-run a dir list job. */
    GList* l = fm_file_info_list_peek_head_link(files);
#endif

    /* cancel running dir listing job if there is any. */
    if(dirlist_job) {
        disconnect(dirlist_job, &DirListJob::finished, this, &Folder::onDirListFinished);
        dirlist_job->cancel();
        dirlist_job = nullptr;
    }

    /* remove all existing files */
#if 0
    if(l) {
        if(g_signal_has_handler_pending(folder, signals[FILES_REMOVED], 0, true)) {
            /* need to emit signal of removal */
            GSList* files_to_del = NULL;
            for(; l; l = l->next) {
                files_to_del = g_slist_prepend(files_to_del, (FmFileInfo*)l->data);
            }
            g_signal_emit(folder, signals[FILES_REMOVED], 0, files_to_del);
            g_slist_free(files_to_del);
        }
        fm_file_info_list_clear(files); /* fm_file_info_unref will be invoked. */
    }
#endif

    /* also re-create a new file monitor */
    // mon = GObjectPtr<GFileMonitor>{fm_monitor_directory(dir_path.gfile().get(), &err), false};
    // FIXME: should we make this cancellable?
    mon = GObjectPtr<GFileMonitor>{
            g_file_monitor_directory(dir_path.gfile().get(), G_FILE_MONITOR_WATCH_MOUNTS, nullptr, &err),
            false
    };

    if(mon) {
        mon_changed.connect(mon.get(), "changed");
    }
    else {
        mon_changed.disconnect();
        qDebug("file monitor cannot be created: %s", err->message);
        g_error_free(err);
    }

    Q_EMIT contentChanged();

    /* run a new dir listing job */
    // FIXME:
    // defer_content_test = fm_config->defer_content_test;

    dirlist_job = new DirListJob(dir_path, defer_content_test ? DirListJob::FAST : DirListJob::DETAILED);
    connect(dirlist_job, &DirListJob::finished, this, &Folder::onDirListFinished);

#if 0
    if(wants_incremental) {
        g_signal_connect(dirlist_job, "files-found", G_CALLBACK(on_dirlist_job_files_found), folder);
    }
    fm_dir_list_job_set_incremental(dirlist_job, wants_incremental);
    g_signal_connect(dirlist_job, "error", G_CALLBACK(on_dirlist_job_error), folder);
#endif

    dirlist_job->setAutoDelete(true);
    // dirlist_job->runAsync();
    QThreadPool::globalInstance()->start(dirlist_job);

    /* also reload filesystem info.
     * FIXME: is this needed? */
    queryFilesystemInfo();
}

#if 0
/**
 * Folder::is_loaded
 * @folder: folder to test
 *
 * Checks if all data for @folder is completely loaded.
 *
 * Before 1.0.0 this call had name Folder::get_is_loaded.
 *
 * Returns: %true is loading of folder is already completed.
 *
 * Since: 0.1.16
 */
bool Folder::is_loaded(FmFolder* folder) {
    return (dirlist_job == NULL);
}

/**
 * Folder::is_valid
 * @folder: folder to test
 *
 * Checks if directory described by @folder exists.
 *
 * Returns: %true if @folder describes a valid existing directory.
 *
 * Since: 1.0.0
 */
bool Folder::is_valid(FmFolder* folder) {
    return (dir_fi != NULL);
}

/**
 * Folder::is_incremental
 * @folder: folder to test
 *
 * Checks if a folder is incrementally loaded.
 * After an FmFolder object is obtained from calling Folder::from_path(),
 * if it's not yet loaded, it begins loading the content of the folder
 * and emits "start-loading" signal. Most of the time, the info of the
 * files in the folder becomes available only after the folder is fully
 * loaded. That means, after the "finish-loading" signal is emitted.
 * Before the loading is finished, Folder::get_files() returns nothing.
 * You can tell if a folder is still being loaded with Folder::is_loaded().
 *
 * However, for some special FmFolder types, such as the ones handling
 * search:// URIs, we want to access the file infos while the folder is
 * still being loaded (the search is still ongoing).
 * The content of the folder grows incrementally and Folder::get_files()
 * returns files currently being loaded even when the folder is not
 * fully loaded. This is what we called incremental.
 * Folder::is_incremental() tells you if the FmFolder has this feature.
 *
 * Returns: %true if @folder is incrementally loaded
 *
 * Since: 1.0.2
 */
bool Folder::is_incremental(FmFolder* folder) {
    return wants_incremental;
}


/**
 * Folder::get_filesystem_info
 * @folder: folder to retrieve info
 * @total_size: pointer to counter of total size of the filesystem
 * @free_size: pointer to counter of free space on the filesystem
 *
 * Retrieves info about total and free space on the filesystem which
 * contains the @folder.
 *
 * Returns: %true if information can be retrieved.
 *
 * Since: 0.1.16
 */
bool Folder::get_filesystem_info(FmFolder* folder, guint64* total_size, guint64* free_size) {
    if(has_fs_info) {
        *total_size = fs_total_size;
        *free_size = fs_free_size;
        return true;
    }
    return false;
}

/* this function is run in GIO thread! */
void on_query_filesystem_info_finished(GObject* src, GAsyncResult* res, FmFolder* folder) {
    GFile* gf = G_FILE(src);
    GError* err = NULL;
    GFileInfo* inf = g_file_query_filesystem_info_finish(gf, res, &err);
    if(!inf) {
        fs_total_size = fs_free_size = 0;
        has_fs_info = false;
        fs_info_not_avail = true;

        /* FIXME: examine unsupported filesystems */

        g_error_free(err);
        goto _out;
    }
    if(g_file_info_has_attribute(inf, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE)) {
        fs_total_size = g_file_info_get_attribute_uint64(inf, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
        fs_free_size = g_file_info_get_attribute_uint64(inf, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
        has_fs_info = true;
    }
    else {
        fs_total_size = fs_free_size = 0;
        has_fs_info = false;
        fs_info_not_avail = true;
    }
    g_object_unref(inf);

_out:
    G_LOCK(query);
    if(fs_size_cancellable) {
        g_object_unref(fs_size_cancellable);
        fs_size_cancellable = NULL;
    }

    filesystem_info_pending = true;
    G_UNLOCK(query);
    /* we have a reference borrowed by async query still */
    G_LOCK(lists);
    queue_update(folder);
    G_UNLOCK(lists);
    g_object_unref(folder);
}

/**
 * Folder::query_filesystem_info
 * @folder: folder to retrieve info
 *
 * Queries to retrieve info about filesystem which contains the @folder if
 * the filesystem supports such query.
 *
 * Since: 0.1.16
 */
void Folder::query_filesystem_info(FmFolder* folder) {
    G_LOCK(query);
    if(!fs_size_cancellable && !fs_info_not_avail) {
        fs_size_cancellable = g_cancellable_new();
        g_file_query_filesystem_info_async(gf,
                                           G_FILE_ATTRIBUTE_FILESYSTEM_SIZE","
                                           G_FILE_ATTRIBUTE_FILESYSTEM_FREE,
                                           G_PRIORITY_LOW, fs_size_cancellable,
                                           (GAsyncReadyCallback)on_query_filesystem_info_finished,
                                           g_object_ref(folder));
    }
    G_UNLOCK(query);
}


/**
 * Folder::block_updates
 * @folder: folder to apply
 *
 * Blocks emitting signals for changes in folder, i.e. if some file was
 * added, changed, or removed in folder after this API, no signal will be
 * sent until next call to Folder::unblock_updates().
 *
 * Since: 1.2.0
 */
void Folder::block_updates(FmFolder* folder) {
    /* g_debug("Folder::block_updates %p", folder); */
    G_LOCK(lists);
    /* just set the flag */
    stop_emission = true;
    G_UNLOCK(lists);
}

/**
 * Folder::unblock_updates
 * @folder: folder to apply
 *
 * Unblocks emitting signals for changes in folder. If some changes were
 * in folder after previous call to Folder::block_updates() then these
 * changes will be sent after this call.
 *
 * Since: 1.2.0
 */
void Folder::unblock_updates(FmFolder* folder) {
    /* g_debug("Folder::unblock_updates %p", folder); */
    G_LOCK(lists);
    stop_emission = false;
    /* query update now */
    queue_update(folder);
    G_UNLOCK(lists);
    /* g_debug("Folder::unblock_updates OK"); */
}

/**
 * Folder::make_directory
 * @folder: folder to apply
 * @name: display name for new directory
 * @error: (allow-none) (out): location to save error
 *
 * Creates new directory in given @folder.
 *
 * Returns: %true in case of success.
 *
 * Since: 1.2.0
 */
bool Folder::make_directory(FmFolder* folder, const char* name, GError** error) {
    GFile* dir, *gf;
    FmPath* path;
    bool ok;

    dir = fm_path_to_gfile(dir_path);
    gf = g_file_get_child_for_display_name(dir, name, error);
    g_object_unref(dir);
    if(gf == NULL) {
        return false;
    }
    ok = g_file_make_directory(gf, NULL, error);
    if(ok) {
        path = fm_path_new_for_gfile(gf);
        if(!_Folder::event_file_added(folder, path)) {
            fm_path_unref(path);
        }
    }
    g_object_unref(gf);
    return ok;
}

void Folder::content_changed(FmFolder* folder) {
    if(has_fs_info && !fs_info_not_avail) {
        Folder::query_filesystem_info(folder);
    }
}

/* NOTE:
 * GFileMonitor has some significant limitations:
 * 1. Currently it can correctly emit unmounted event for a directory.
 * 2. After a directory is unmounted, its content changes.
 *    Inotify does not fire events for this so a forced reload is needed.
 * 3. If a folder is empty, and later a filesystem is mounted to the
 *    folder, its content should reflect the content of the newly mounted
 *    filesystem. However, GFileMonitor and inotify do not emit events
 *    for this case. A forced reload might be needed for this case as well.
 * 4. Some limitations come from Linux/inotify. If FAM/gamin is used,
 *    the condition may be different. More testing is needed.
 */
void on_mount_added(GVolumeMonitor* vm, GMount* mount, gpointer user_data) {
    /* If a filesystem is mounted over an existing folder,
     * we need to refresh the content of the folder to reflect
     * the changes. Besides, we need to create a new GFileMonitor
     * for the newly-mounted filesystem as the inode already changed.
     * GFileMonitor cannot detect this kind of changes caused by mounting.
     * So let's do it ourselves. */

    GFile* gfile = g_mount_get_root(mount);
    /* g_debug("FmFolder::mount_added"); */
    if(gfile) {
        GHashTableIter it;
        FmPath* path;
        FmFolder* folder;
        FmPath* mounted_path = fm_path_new_for_gfile(gfile);
        g_object_unref(gfile);

        G_LOCK(hash);
        g_hash_table_iter_init(&it, hash);
        while(g_hash_table_iter_next(&it, (gpointer*)&path, (gpointer*)&folder)) {
            if(path == mounted_path) {
                queue_reload(folder);
            }
            else if(fm_path_has_prefix(path, mounted_path)) {
                /* see if currently cached folders are below the mounted path.
                 * Folders below the mounted folder are removed.
                 * FIXME: should we emit "removed" signal for them, or
                 * keep the folders and only reload them? */
                /* g_signal_emit(folder, signals[REMOVED], 0); */
                queue_reload(folder);
            }
        }
        G_UNLOCK(hash);
        fm_path_unref(mounted_path);
    }
}

void on_mount_removed(GVolumeMonitor* vm, GMount* mount, gpointer user_data) {
    /* g_debug("FmFolder::mount_removed"); */

    /* NOTE: gvfs does not emit unmount signals for remote folders since
     * GFileMonitor does not support remote filesystems at all. We do fake
     * file monitoring with FmDummyMonitor dirty hack.
     * So here is the side effect, no unmount notifications.
     * We need to generate the signal ourselves. */

    GFile* gfile = g_mount_get_root(mount);
    if(gfile) {
        GSList* dummy_monitor_folders = NULL, *l;
        GHashTableIter it;
        FmPath* path;
        FmFolder* folder;
        FmPath* mounted_path = fm_path_new_for_gfile(gfile);
        g_object_unref(gfile);

        G_LOCK(hash);
        g_hash_table_iter_init(&it, hash);
        while(g_hash_table_iter_next(&it, (gpointer*)&path, (gpointer*)&folder)) {
            if(fm_path_has_prefix(path, mounted_path)) {
                /* see if currently cached folders are below the mounted path.
                 * Folders below the mounted folder are removed. */
                if(FM_IS_DUMMY_MONITOR(mon)) {
                    dummy_monitor_folders = g_slist_prepend(dummy_monitor_folders, folder);
                }
            }
        }
        G_UNLOCK(hash);
        fm_path_unref(mounted_path);

        for(l = dummy_monitor_folders; l; l = l->next) {
            folder = FM_FOLDER(l->data);
            g_object_ref(folder);
            g_signal_emit_by_name(mon, "changed", gf, NULL, G_FILE_MONITOR_EVENT_UNMOUNTED);
            /* FIXME: should we emit a fake deleted event here? */
            /* g_signal_emit_by_name(mon, "changed", gf, NULL, G_FILE_MONITOR_EVENT_DELETED); */
            g_object_unref(folder);
        }
        g_slist_free(dummy_monitor_folders);
    }
}

#endif
} // namespace Fm2