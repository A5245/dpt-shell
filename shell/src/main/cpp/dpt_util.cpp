//
// Created by luoyesiqiu
//

#include <libgen.h>
#include <ctime>
#include <elf.h>
#include <pthread.h>
#include <errno.h>
#include <dlfcn.h>
#include <mz_strm.h>
#include <string>
#include <algorithm>
#include <cctype>

#include "dpt_util.h"
#include "common/dpt_log.h"

using namespace dpt;

DPT_DATA_SECTION uint8_t DATA_R_FLAG[] = "r";

std::string find_so_path(const char* so_name) {
    const int MAX_READ_LINE = 10 * 1024;
    char maps_path[128] = {0};
    snprintf(maps_path, ARRAY_LENGTH(maps_path), AY_OBFUSCATE("/proc/%d/maps"), getpid());
    FILE *fp = fopen(maps_path, "r");

#ifdef __LP64__
    const char* maps_line_fmt = AY_OBFUSCATE("%*llx-%*llx %*s %*llx %*s %*s %s");
#else
    const char* maps_line_fmt = AY_OBFUSCATE("%*x-%*x %*s %*x %*s %*s %s");
#endif

    std::string so_path_result = {};
    if (fp != nullptr) {
        char line[512] = {0};
        int read_line = 0;
        while (fgets(line, sizeof(line), fp) != nullptr) {
            if (read_line++ >= MAX_READ_LINE) {
                break;
            }
            char item_name[128] = {0};

            int ret = sscanf(line, maps_line_fmt, item_name);

            if(ret != 1) {
                continue;
            }

            if(strstr(item_name, so_name) != nullptr) {
                so_path_result = item_name;
                break;
            }
        } // while
        fclose(fp);
    }

    return so_path_result;
}

bool checkWebViewInFilename(const std::string& path) {
    size_t lastSlashPos = path.find_last_of("\\/");
    std::string filename;

    if (lastSlashPos != std::string::npos) {
        filename = path.substr(lastSlashPos + 1);
    } else {
        filename = path;
    }

    std::string lowercaseFilename;
    lowercaseFilename.resize(filename.size());
    std::transform(
            filename.begin(), filename.end(),
            lowercaseFilename.begin(),
            [](unsigned char c) { return std::tolower(c); }
    );

    return lowercaseFilename.find("webview") != std::string::npos && lowercaseFilename.find("vdex") != std::string::npos;;
}

size_t dpt_readlink(int fd, char *result_path,size_t path_max_len) {
    char link_path[128] = {0};
    snprintf(link_path,sizeof(link_path),"/proc/%d/fd/%d",getpid(),fd);
    return readlink(link_path,result_path,path_max_len);
}

int dpt_mprotect(void *start,void *end,int prot) {
    uintptr_t start_addr = PAGE_START((uintptr_t)start);
    uintptr_t end_addr = PAGE_START((uintptr_t)end - 1) + getpagesize();
    size_t size = end_addr - start_addr;

    if (0 != mprotect((void *)start_addr, size, prot)) {
        DLOGW("mprotect 0x%" PRIxPTR  "-0x%" PRIxPTR  " fail, pagesize: %d, err: %s", start_addr, end_addr,
              getpagesize(),
              strerror(errno));
        return -1;
    }
    return 0;
}

static int separate_dex_number(std::string *str) {
    int sum = 0;
    int mul = 1;
    for(auto it = str->cend();it >= str->cbegin();it--){
        if(isdigit(*it)){
            int number = *it - '0';
            sum += (number * mul);
            mul *= 10;
        }
        else{
            if(sum != 0 ) break;
        }
    }
    return sum;
}
/**
 * Get dex index from dex location
 * e.g. base.apk!classes2.dex will get 1
 */
int parse_dex_number(std::string *location) {
    int raw_dex_index = 1;
    if (location->rfind(".dex") != std::string::npos) {
        size_t sep = location->rfind('!');
        if(sep != std::string::npos){
            raw_dex_index = separate_dex_number(location);
        }
        else{
            sep = location->rfind(':');
            if(sep != std::string::npos){
                raw_dex_index = separate_dex_number(location);
            }
        }
    } else {
        raw_dex_index = 1;
    }

    return raw_dex_index - 1;
}

void parseClassName(const char *src, char *dest) {
    for (int i = 0; *(src + i) != '\0'; i++) {
        if (*(src + i) == '.') {
            dest[i] = '/';
        } else {
            *(dest + i) = *(src + i);
        }
    }
}

void getClassName(JNIEnv *env,jobject obj,char *destClassName,size_t max_len) {
    jclass objCls = env->GetObjectClass(obj);
    reflect::java_lang_Class cls(env,objCls);
    jstring classNameInner = cls.getName();

    const char *classNameInnerChs = env->GetStringUTFChars(classNameInner,nullptr);

    snprintf(destClassName,max_len,"%s",classNameInnerChs);

    env->ReleaseStringUTFChars(classNameInner,classNameInnerChs);
}

jclass getContextClass(JNIEnv *env) {
    if (g_ContextClass == nullptr) {
        jclass ContextClass = env->FindClass("android/content/Context");
        g_ContextClass = (jclass) env->NewGlobalRef(ContextClass);
    }
    return g_ContextClass;
}

AAssetManager *getAssetMgr(JNIEnv *env, jobject assetManager) {
    if (g_AssetMgrInstance == nullptr) {
        g_AssetMgrInstance = AAssetManager_fromJava(env, assetManager);
    }
    return g_AssetMgrInstance;
}

AAsset *getAsset(JNIEnv *env, jobject context, const char *filename) {
    if (context != nullptr) {
        jclass contextClass = getContextClass(env);
        jmethodID getAssetsId = env->GetMethodID(contextClass, "getAssets",
                                                 "()Landroid/content/res/AssetManager;");
        jobject assetManagerObj = env->CallObjectMethod(context, getAssetsId);
        AAssetManager *aAssetManager = getAssetMgr(env, assetManagerObj);
        if (aAssetManager != nullptr) {
            AAsset *aAsset = AAssetManager_open(g_AssetMgrInstance,
                                                filename,
                                                AASSET_MODE_BUFFER);
            return aAsset;
        }
    }
    return nullptr;
}

void getSourceDir(JNIEnv *env, char *sourceDirOut, size_t max_out_len) {
    reflect::android_app_ActivityThread activityThread(env);
    jobject mBoundApplicationObj = activityThread.getBoundApplication();

    reflect::android_app_ActivityThread::AppBindData appBindData(env,mBoundApplicationObj);
    jobject appInfoObj = appBindData.getAppInfo();

    reflect::android_content_pm_ApplicationInfo applicationInfo(env,appInfoObj);
    auto sourceDir = applicationInfo.getSourceDir();

    const char *sourceDirChs = env->GetStringUTFChars(sourceDir,nullptr);
    snprintf(sourceDirOut, max_out_len, "%s", sourceDirChs);

    DLOGD("source dir: %s", sourceDirOut);
}

void getDataDir(JNIEnv *env,char *dataDirOut,size_t max_out_len) {
    reflect::android_app_ActivityThread activityThread(env);
    jobject mBoundApplicationObj = activityThread.getBoundApplication();

    reflect::android_app_ActivityThread::AppBindData appBindData(env,mBoundApplicationObj);
    jobject appInfoObj = appBindData.getAppInfo();

    reflect::android_content_pm_ApplicationInfo applicationInfo(env,appInfoObj);
    auto dataDir = applicationInfo.getDataDir();

    const char *dataDirChs = env->GetStringUTFChars(dataDir,nullptr);
    snprintf(dataDirOut, max_out_len , "%s",dataDirChs);

    DLOGD("data dir: %s",dataDirOut);
}

jstring getSourceDirExport(JNIEnv *env,jclass __unused) {
    char sourceDirChs[512] = {0};
    getSourceDir(env, sourceDirChs, ARRAY_LENGTH(sourceDirChs));

    return env->NewStringUTF(sourceDirChs);
}

void getCompressedDexesPath(JNIEnv *env,char *outDexZipPath,size_t max_len) {
    char dataDir[256] = {0};
    getDataDir(env,dataDir, ARRAY_LENGTH(dataDir));
    snprintf(outDexZipPath,max_len,"%s/%s/%s",dataDir,CACHE_DIR,DEXES_ZIP_NAME);
}

void getCodeCachePath(JNIEnv *env,char *outCodeCachePath,size_t max_len) {
    char dataDir[256] = {0};
    getDataDir(env,dataDir, ARRAY_LENGTH(dataDir));
    snprintf(outCodeCachePath,max_len,"%s/%s/",dataDir,CACHE_DIR);
}

jstring getCompressedDexesPathExport(JNIEnv *env,jclass __unused) {
    char dexesPath[256] = {0};
    getCompressedDexesPath(env, dexesPath, ARRAY_LENGTH(dexesPath));
    return env->NewStringUTF(dexesPath);
}

static uint32_t readZipLength(const uint8_t *data, size_t size) {
    if (size < 4) return 0;

    uint32_t length = 0;
    memcpy(&length, data + size - 4, 4);

    // Byte swapping for little-endian format
    length = (length >> 24) | ((length & 0x00FF0000) >> 8) | ((length & 0x0000FF00) << 8) | (length << 24);

    return length;
}

DPT_ENCRYPT static void writeDexAchieve(const char *dexAchievePath, void *package_addr, size_t package_size) {
    DLOGD("zipCode open = %s",dexAchievePath);
    FILE *fp = fopen(dexAchievePath, "wb");
    if(fp != nullptr){
        uint64_t dex_files_size = 0;
        void *dexFilesData = nullptr;
        bool needFree = read_zip_file_entry(package_addr, package_size, COMBINE_DEX_FILES_NAME_IN_ZIP, &dexFilesData, &dex_files_size);
        if (dexFilesData != nullptr) {
            DLOGD("Read classes.dex of size: %lu", (unsigned long)dex_files_size);
            uint32_t zipDataLen = readZipLength((uint8_t *)dexFilesData, dex_files_size);
            DLOGD("Extracted zip data length: %u", (unsigned int)zipDataLen);

            if (zipDataLen > 0 && dex_files_size > zipDataLen + 4) {
                // Zip file data starts at the end of classes.dex minus the zip length and 4 bytes for the length field
                uint8_t *zipDataStart = (uint8_t *)dexFilesData + (dex_files_size - zipDataLen - 4);
                fwrite(zipDataStart, 1, zipDataLen, fp);
                DLOGD("Zip file extracted and written successfully.");
            } else {
                DLOGE("Invalid zip data length: %u. dex_files_size: %lu", (unsigned int)zipDataLen, (unsigned long)dex_files_size);
            }
        } else {
            DLOGE("Failed to read classes.dex.");
        }
        fclose(fp);
        if(needFree) {
            DPT_FREE(dexFilesData);
        }
    }
    else {
        DLOGE("WTF! zipCode write fail: %s", strerror(errno));
    }
}

DPT_ENCRYPT void extractDexesInNeeded(JNIEnv *env, void *package_addr, size_t package_size) {
    char compressedDexesPathChs[256] = {0};
    getCompressedDexesPath(env,compressedDexesPathChs, ARRAY_LENGTH(compressedDexesPathChs));

    char codeCachePathChs[256] = {0};
    getCodeCachePath(env,codeCachePathChs, ARRAY_LENGTH(codeCachePathChs));

    if(access(codeCachePathChs, F_OK) == 0){
        if(access(compressedDexesPathChs, F_OK) != 0) {
            writeDexAchieve(compressedDexesPathChs, package_addr, package_size);
            chmod(compressedDexesPathChs,0444);
            DLOGI("%s write finish", compressedDexesPathChs);

        }
        else {
            DLOGI("dex files is achieved!");
        }
    }
    else {
        if(mkdir(codeCachePathChs,0775) == 0){
            writeDexAchieve(compressedDexesPathChs, package_addr, package_size);
            chmod(compressedDexesPathChs,0444);
        }
        else {
            DLOGE("WTF! extractDexes cannot make code_cache directory!");
        }
    }
}

DPT_ENCRYPT static void load_zip_by_mmap(const char* zip_file_path,void **zip_addr,size_t *zip_size) {
    int fd = open(zip_file_path,O_RDONLY);
    if(fd <= 0){
        DLOGE("cannot open file!");
        return;
    }
    struct stat fst;
    fstat(fd,&fst);
    const int page_size = sysconf(_SC_PAGESIZE);
    const size_t need_zip_size = (fst.st_size / page_size) * page_size + page_size;
    DLOGD("fst.st_size: " FMT_INT64_T ", need size: %zu", fst.st_size, need_zip_size);
    *zip_addr = mmap64(nullptr,
                       need_zip_size,
                       PROT_READ ,
                       MAP_PRIVATE,
                       fd,
                       0);
    *zip_size = fst.st_size;

    DLOGD("addr: " FMT_POINTER " size: %zu", (uintptr_t)*zip_addr, *zip_size);
}

static void load_zip(const char* zip_file_path,void **zip_addr,size_t *zip_size) {
    DLOGD("by mmap");
    load_zip_by_mmap(zip_file_path, zip_addr, zip_size);

    DLOGD("start: %p size: %zu", *zip_addr,*zip_size);
}

void load_package(JNIEnv *env, void **package_addr, size_t *package_size) {
    char sourceDirChs[512] = {0};
    getSourceDir(env, sourceDirChs, ARRAY_LENGTH(sourceDirChs));
    load_zip(sourceDirChs, package_addr, package_size);

}

void unload_package(void *package_addr,size_t package_size) {
    if(package_addr != nullptr) {
        munmap(package_addr, package_size);
        DLOGD("addr: " FMT_POINTER " size: %zu", (uintptr_t)package_addr, package_size);
    }
}

DPT_ENCRYPT bool read_zip_file_entry(void* zip_addr,off_t zip_size,const char* entry_name,void **entry_addr,uint64_t *entry_size) {
    DLOGD("prepare read file: %s",entry_name);

    bool needFree = false;

    void *mem_stream = mz_stream_mem_create();
    mz_stream_mem_set_buffer(mem_stream, zip_addr, zip_size);
    mz_stream_open(mem_stream, nullptr, MZ_OPEN_MODE_READ);

    void *zip_handle = mz_zip_create();
    int32_t err = mz_zip_open(zip_handle, mem_stream, MZ_OPEN_MODE_READ);

    if(err == MZ_OK){
        err = mz_zip_goto_first_entry(zip_handle);
        while (err == MZ_OK) {
            mz_zip_file *file_info = nullptr;
            err = mz_zip_entry_get_info(zip_handle, &file_info);

            if (err == MZ_OK) {
                if (strncmp(file_info->filename, entry_name, 256) == 0) {
                    DLOGD("found entry name = %s,file size = " FMT_INT64_T,
                          file_info->filename,
                          file_info->uncompressed_size);
                    if(file_info->uncompressed_size == 0) {
                        *entry_addr = nullptr;
                        *entry_size = 0;
                        return false;
                    }

                    err = mz_zip_entry_read_open(zip_handle, 0, nullptr);
                    if (err != MZ_OK) {
                        DLOGW("not prepared: %d", err);
                        continue;
                    }
                    needFree = true;
                    DLOGD("compress method is: %d",
                          file_info->compression_method);

                    *entry_addr = calloc(file_info->uncompressed_size + 1, 1);
                    DLOGD("start read: %s", file_info->filename);

                    __unused size_t bytes_read = mz_zip_entry_read(zip_handle, *entry_addr,
                                                          file_info->uncompressed_size);

                    DLOGD("reading entry: %s,read size: %zu", entry_name,
                          bytes_read);

                    *entry_size = (file_info->uncompressed_size);

                    goto tail;
                } // strncmp
            }
            else{
                DLOGW("get entry info err: %d",err);
                break;
            }
            err = mz_zip_goto_next_entry(zip_handle);
        } // while
    }
    else{
        DLOGW("zip open fail: %d",err);
    } // zip open

    tail: {
        return needFree;
    }
}

void get_elf_section(Elf_Shdr *target, const char *elf_path, const char *sh_name) {
    if(elf_path == NULL) {
        return;
    }

    FILE *elf_fp = fopen(elf_path, (char *)DATA_R_FLAG);
    if(!elf_fp) {
        DLOGW("cannot open elf file: %s", elf_path);
        return;
    }

    fseek(elf_fp, 0L, SEEK_END);
    size_t lib_size = ftell(elf_fp);
    fseek(elf_fp, 0L, SEEK_SET);

    uint8_t *data = (uint8_t *) calloc(lib_size, 1);
    fread(data, 1, lib_size, elf_fp);

    uint8_t *elf_bytes_data = data;

    Elf_Ehdr *ehdr = (Elf_Ehdr *) elf_bytes_data;
    Elf_Shdr *shdr = (Elf_Shdr *) (((uint8_t *) elf_bytes_data) + ehdr->e_shoff);
    Elf_Half shstrndx = ehdr->e_shstrndx;

    //seek to .shstr
    Elf_Shdr *shstr_shdr = (Elf_Shdr *)((uint8_t *) shdr + sizeof(Elf_Shdr) * shstrndx);

    uint8_t *shstr_addr = elf_bytes_data + shstr_shdr->sh_offset;

    bool found = false;
    for (int i = 0; i < ehdr->e_shnum; i++) {
        const char *section_name = reinterpret_cast<const char *>((char *)shstr_addr + shdr->sh_name);
        if (strcmp(section_name, sh_name) == 0) {
            memcpy(target, shdr, sizeof(Elf_Shdr));
            DLOGD("find section: %s", sh_name);
            found = true;
            break;
        }
        shdr++;
    }

    if(!found) {
        DLOGE("cannot find section: %s", sh_name);
    }
    free(data);
    fclose(elf_fp);
}

DPT_ENCRYPT void find_symbol_in_elf_file(const char *elf_file, char *symbol_name_out, size_t max_out_len, int keyword_count, ...) {
    FILE *elf_fp = fopen(elf_file, "r");

    char* item_value = nullptr;

    if(elf_fp) {
        fseek(elf_fp, 0L, SEEK_END);
        size_t lib_size = ftell(elf_fp);
        fseek(elf_fp, 0L, SEEK_SET);

        uint8_t *lib_buffer = (uint8_t *) calloc(lib_size, 1);
        fread(lib_buffer, 1, lib_size, elf_fp);

        uint8_t *elf_bytes_data = lib_buffer;
        Elf_Ehdr *ehdr = (Elf_Ehdr*)elf_bytes_data;

        Elf_Shdr *shdr = (Elf_Shdr *)(((uint8_t*) elf_bytes_data) + ehdr->e_shoff);

        va_list kw_list;

        for (int i = 0; i < ehdr->e_shnum;i++) {
            if(shdr->sh_type == SHT_STRTAB) {
                const char* str_base = (char *)((uint8_t*)elf_bytes_data + shdr->sh_offset);
                char* ptr = (char *)str_base;

                for(;ptr < (str_base + shdr->sh_size);){
                    item_value = ptr;
                    size_t item_len = strlen(item_value);
                    ptr += (item_len + 1);

                    if(item_len == 0){
                        continue;
                    }
                    int match_count = 0;
                    va_start(kw_list,keyword_count);
                    for(int n = 0;n < keyword_count;n++){
                        const char *keyword = va_arg(kw_list, const char*);
                        if(strstr(item_value,keyword)){
                            match_count++;
                        }
                    }
                    va_end(kw_list);
                    if(match_count == keyword_count){
                        snprintf(symbol_name_out, max_out_len, "%s", item_value);
                        goto tail;
                    }
                }
                break;
            }

            shdr++;
        } //for
        tail: {
        DPT_FREE(lib_buffer);
        fclose(elf_fp);
    };
    } // if
}

DPT_ENCRYPT int find_in_maps(int count,...) {
    const int MAX_READ_LINE = 10 * 1024;
    char maps_path[128] = {0};
    snprintf(maps_path, ARRAY_LENGTH(maps_path), AY_OBFUSCATE("/proc/%d/maps"), getpid());
    FILE *fp = fopen(maps_path, "r");
    int found = 0;

#ifdef __LP64__
    const char* maps_line_fmt = AY_OBFUSCATE("%*llx-%*llx %*s %*llx %*s %*s %s");
#else
    const char* maps_line_fmt = AY_OBFUSCATE("%*x-%*x %*s %*x %*s %*s %s");
#endif

    if (fp != nullptr) {
        char line[512] = {0};
        int read_line = 0;
        va_list ap;
        while (fgets(line, sizeof(line), fp) != nullptr) {
            if (read_line++ >= MAX_READ_LINE) {
                break;
            }
            char item_name[128] = {0};

            int ret = sscanf(line, maps_line_fmt, item_name);

            if(ret != 1) {
                continue;
            }
            va_start(ap,count);

            for(int i = 0;i < count;i++) {
                const char *arg = va_arg(ap,const char *);
                if(dpt_strstr(item_name, arg) != nullptr) {
                    DLOGD("found %s in %s",arg,item_name);
                    found++;
                }
            }
            va_end(ap);

        }
    }

    return found;
}

DPT_ENCRYPT int find_in_threads_list(int count,...) {
    char task_path[128] = {0};
    pid_t pid = getpid();
    const char *path_fmt = AY_OBFUSCATE("/proc/%d/task");
    snprintf(task_path, ARRAY_LENGTH(task_path), path_fmt, pid);
    DIR *task_dir;
    if((task_dir = opendir(task_path)) == nullptr) {
        return 0;
    }

    int match_count = 0;

    struct dirent *de;
    va_list ap;
    const char *stat = AY_OBFUSCATE("stat");
    while ((de = readdir(task_dir)) != nullptr) {
        if(isdigit(de->d_name[0])) {
            int tid = atoi(de->d_name);
            if(tid == pid) {
                continue;
            }
            char stat_path[256] = {0};
            snprintf(stat_path, ARRAY_LENGTH(stat_path),"%s/%d/%s", task_path, tid, stat);
            FILE *fp = fopen(stat_path,"r");
            char buf[256] = {0};
            if(fp) {
                fgets(buf,256,fp);

                char *t_name = nullptr;
                for(size_t i = 0; i < strnlen(buf,256);i++) {
                    if(buf[i] == '(') {
                        t_name = &buf[i + 1];
                    }

                    if(buf[i] == ')') {
                        buf[i] = '\0';
                        break;
                    }

                }

                if(t_name != nullptr) {
                    va_start(ap,count);
                    for (int i = 0; i < count; i++) {
                        const char *arg = va_arg(ap, const char *);
                        if (strncmp(t_name, arg, 256) == 0) {
                            DLOGD("match thread name: %s", t_name);
                            match_count++;
                        }
                    }
                    va_end(ap);
                }
                fclose(fp);
            }
        }
    }

    if(task_dir) {
        closedir(task_dir);
    }
    return match_count;
}

void appendLog(const char* log){
    FILE *fp = fopen("nlog.log","aw");
    if(NULL != fp){
        fwrite(log,1,strlen(log),fp);
        fwrite("\n",1,1,fp);
        fclose(fp);
    }
}

void printTime(__unused const char* msg,__unused clock_t start){
    __unused clock_t end = clock();
    DLOGD("%s %lf",msg,(double)(end - start) / CLOCKS_PER_SEC);
}
