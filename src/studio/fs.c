// MIT License

// Copyright (c) 2017 Vadim Grigoruk @nesbox // grigoruk@gmail.com

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "studio.h"
#include "fs.h"
#include "net.h"

#if defined(BAREMETALPI) || defined(_3DS)
  #ifdef EN_DEBUG
    #define dbg(...) printf(__VA_ARGS__)
  #else
    #define dbg(...)
  #endif
#endif

#if defined(BAREMETALPI)
#include "../../circle-stdlib/libs/circle/addon/fatfs/ff.h"
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#if defined(__TIC_WINDOWS__)
#include <direct.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

static const char* PublicDir = TIC_HOST;

struct FileSystem
{
    char dir[TICNAME_MAX];
    char work[TICNAME_MAX];
    Net* net;
};

#if defined(__EMSCRIPTEN__)
void syncfs()
{
    EM_ASM({Module.syncFSRequests++;});
}
#endif 

const char* fsGetRootFilePath(FileSystem* fs, const char* name)
{
    static char path[TICNAME_MAX];

    sprintf(path, "%s%s", fs->dir, name);

#if defined(__TIC_WINDOWS__)
    char* ptr = path;
    while (*ptr)
    {
        if (*ptr == '/') *ptr = '\\';
        ptr++;
    }
#endif

    return path;
}

const char* fsGetFilePath(FileSystem* fs, const char* name)
{
    static char path[TICNAME_MAX];

    if(*name == '/')
        strcpy(path, name + 1);
    else if(strlen(fs->work))
        sprintf(path, "%s/%s", fs->work, name);
    else 
        strcpy(path, name);

    return fsGetRootFilePath(fs, path);
}

static bool isRoot(FileSystem* fs)
{
    return fs->work[0] == '\0';
}

static bool isPublicRoot(FileSystem* fs)
{
    return strcmp(fs->work, PublicDir) == 0;
}

static bool isPublic(FileSystem* fs)
{
    return memcmp(fs->work, PublicDir, sizeof PublicDir - 1) == 0;
}

bool fsIsInPublicDir(FileSystem* fs)
{
    return isPublic(fs);
}

#if defined(__TIC_WINDOWS__)

typedef wchar_t FsString;

#define __S(x) L ## x 
#define _S(x) __S(x)

static const FsString* utf8ToString(const char* str)
{
    FsString* wstr = malloc(TICNAME_MAX * sizeof(FsString));

    MultiByteToWideChar(CP_UTF8, 0, str, TICNAME_MAX, wstr, TICNAME_MAX);

    return wstr;
}

static const char* stringToUtf8(const FsString* wstr)
{
    char* str = malloc(TICNAME_MAX * sizeof(char));

    WideCharToMultiByte(CP_UTF8, 0, wstr, TICNAME_MAX, str, TICNAME_MAX, 0, 0);

    return str;
}

#define freeString(S) free((void*)S)


#define TIC_DIR _WDIR
#define tic_dirent _wdirent
#define tic_stat_struct _stat

#define tic_opendir _wopendir
#define tic_readdir _wreaddir
#define tic_closedir _wclosedir
#define tic_rmdir _wrmdir
#define tic_stat _wstat
#define tic_remove _wremove
#define tic_fopen _wfopen
#define tic_mkdir(name) _wmkdir(name)
#define tic_strcpy wcscpy
#define tic_strcat wcscat

#else

typedef char FsString;

#define _S(x) (x)

#define utf8ToString(S) (S)
#define stringToUtf8(S) (S)

#define freeString(S)

#define TIC_DIR DIR
#define tic_dirent dirent
#define tic_stat_struct stat

#define tic_opendir opendir
#define tic_readdir readdir
#define tic_closedir closedir
#define tic_rmdir rmdir
#define tic_stat stat
#define tic_remove remove
#define tic_fopen fopen
#define tic_mkdir(name) mkdir(name, 0700)
#define tic_strcpy strcpy
#define tic_strcat strcat

#endif

typedef struct
{
    ListCallback item;
    DoneCallback done;
    void* data;
} NetDirData;

static lua_State* netLuaInit(u8* buffer, s32 size)
{
    if (buffer && size)
    {
        char* script = calloc(1, size + 1);
        memcpy(script, buffer, size);

        lua_State* lua = luaL_newstate();

        if(lua)
        {
            if(luaL_loadstring(lua, script) == LUA_OK && lua_pcall(lua, 0, LUA_MULTRET, 0) == LUA_OK)
                return lua;

            else lua_close(lua);
        }

        free(script);
    }

    return NULL;
}

static void onDirResponse(const HttpGetData* netData)
{
    NetDirData* netDirData = (NetDirData*)netData->calldata;

    if(netData->type == HttpGetDone)
    {
        lua_State* lua = netLuaInit(netData->done.data, netData->done.size);

        if (lua)
        {
            {
                lua_getglobal(lua, "folders");

                if (lua_type(lua, -1) == LUA_TTABLE)
                {
                    s32 count = (s32)lua_rawlen(lua, -1);

                    for (s32 i = 1; i <= count; i++)
                    {
                        lua_geti(lua, -1, i);

                        {
                            lua_getfield(lua, -1, "name");
                            if (lua_isstring(lua, -1))
                                netDirData->item(lua_tostring(lua, -1), NULL, 0, netDirData->data, true);

                            lua_pop(lua, 1);
                        }

                        lua_pop(lua, 1);
                    }
                }

                lua_pop(lua, 1);
            }

            {
                lua_getglobal(lua, "files");

                if (lua_type(lua, -1) == LUA_TTABLE)
                {
                    s32 count = (s32)lua_rawlen(lua, -1);

                    for (s32 i = 1; i <= count; i++)
                    {
                        lua_geti(lua, -1, i);

                        char hash[TICNAME_MAX];
                        char name[TICNAME_MAX];

                        {
                            lua_getfield(lua, -1, "hash");
                            if (lua_isstring(lua, -1))
                                strcpy(hash, lua_tostring(lua, -1));

                            lua_pop(lua, 1);
                        }

                        {
                            lua_getfield(lua, -1, "name");

                            if (lua_isstring(lua, -1))
                                strcpy(name, lua_tostring(lua, -1));

                            lua_pop(lua, 1);
                        }

                        {
                            lua_getfield(lua, -1, "id");

                            if (lua_isinteger(lua, -1))
                                netDirData->item(name, hash, (s32)lua_tointeger(lua, -1), netDirData->data, false);

                            lua_pop(lua, 1);
                        }

                        lua_pop(lua, 1);
                    }
                }

                lua_pop(lua, 1);
            }

            lua_close(lua);
        }
    }

    switch (netData->type)
    {
    case HttpGetDone:
    case HttpGetError:
        netDirData->done(netDirData->data);
        free(netDirData);
        break;
    }
}

static void enumFiles(FileSystem* fs, const char* path, ListCallback callback, void* data, bool folder)
{
#if defined(BAREMETALPI)
    dbg("enumFiles %s", path);

        if (path && *path) {
        // ok
    }
    else
    {
        return;
    }

    static char path2[TICNAME_MAX];
    strcpy(path2, path);

    if (path2[strlen(path2) - 1] == '/')    // one character
                path2[strlen(path2) - 1] = 0;

    dbg("enumFiles Real %s", path2);


    DIR Directory;
    FILINFO FileInfo;
    FRESULT Result = f_findfirst (&Directory, &FileInfo, path2, "*");
    dbg("enumFilesRes %d", Result);

    for (unsigned i = 0; Result == FR_OK && FileInfo.fname[0]; i++)
    {
        bool check = FileInfo.fattrib & (folder ? AM_DIR : AM_ARC);

        if (check &&  !(FileInfo.fattrib & (AM_HID | AM_SYS)))
        {
            bool result = callback(FileInfo.fname, NULL, 0, data, folder);

            if (!result)
            {
                break;
            }
        }

        Result = f_findnext (&Directory, &FileInfo);
    }

#else
    TIC_DIR *dir = NULL;
    struct tic_dirent* ent = NULL;

    const FsString* pathString = utf8ToString(path);

    if ((dir = tic_opendir(pathString)) != NULL)
    {
        FsString fullPath[TICNAME_MAX];
        struct tic_stat_struct s;
        
        while ((ent = tic_readdir(dir)) != NULL)
        {
            if(*ent->d_name != _S('.'))
            {
                tic_strcpy(fullPath, pathString);
                tic_strcat(fullPath, ent->d_name);

                if(tic_stat(fullPath, &s) == 0 && folder ? S_ISDIR(s.st_mode) : S_ISREG(s.st_mode))
                {
                    const char* name = stringToUtf8(ent->d_name);
                    bool result = callback(name, NULL, 0, data, folder);
                    freeString(name);

                    if(!result) break;
                }
            }
        }

        tic_closedir(dir);
    }

    freeString(pathString);
#endif
}

void fsEnumFilesAsync(FileSystem* fs, ListCallback onItem, DoneCallback onDone, void* data)
{
    if (isRoot(fs) && !onItem(PublicDir, NULL, 0, data, true))
    {
        onDone(data);
        return;
    }

    if(isPublic(fs))
    {
        char request[TICNAME_MAX];
        sprintf(request, "/api?fn=dir&path=%s", fs->work + sizeof(TIC_HOST));

        NetDirData netDirData = { onItem, onDone, data };
        netGet(fs->net, request, onDirResponse, OBJCOPY(netDirData));

        return;
    }

    const char* path = fsGetFilePath(fs, "");

    enumFiles(fs, path, onItem, data, true);
    enumFiles(fs, path, onItem, data, false);

    onDone(data);
}

bool fsDeleteDir(FileSystem* fs, const char* name)
{
#if defined(BAREMETALPI)
    // TODO BAREMETALPI
    dbg("fsDeleteDir %s", name);
    return 0;
#else
#if defined(__TIC_WINDOWS__)
    const char* path = fsGetFilePath(fs, name);

    const FsString* pathString = utf8ToString(path);
    bool result = tic_rmdir(pathString);
    freeString(pathString);

#else
    bool result = rmdir(fsGetFilePath(fs, name));
#endif

#if defined(__EMSCRIPTEN__)
    syncfs();
#endif  

    return result;
#endif
}

bool fsDeleteFile(FileSystem* fs, const char* name)
{
#if defined(BAREMETALPI)
    dbg("fsDeleteFile %s", name);
    // TODO BAREMETALPI
    return false;
#else
    const char* path = fsGetFilePath(fs, name);

    const FsString* pathString = utf8ToString(path);
    bool result = tic_remove(pathString);
    freeString(pathString);

#if defined(__EMSCRIPTEN__)
    syncfs();
#endif  

    return result;
#endif
}

void fsHomeDir(FileSystem* fs)
{
    memset(fs->work, 0, sizeof fs->work);
}

void fsDirBack(FileSystem* fs)
{
    if(isPublicRoot(fs))
    {
        fsHomeDir(fs);
        return;
    }

    char* start = fs->work;
    char* ptr = start + strlen(fs->work);

    while (ptr > start && *ptr != '/') ptr--;
    while (*ptr) *ptr++ = '\0';
}

void fsGetDir(FileSystem* fs, char* dir)
{
    strcpy(dir, fs->work);
}

void fsChangeDir(FileSystem* fs, const char* dir)
{
    if(strlen(fs->work))
        strcat(fs->work, "/");
                
    strcat(fs->work, dir);
}

typedef struct
{
    char* name;
    bool found;
    IsDirCallback done;
    void* data;

} EnumPublicDirsData;

static bool onEnumPublicDirs(const char* name, const char* info, s32 id, void* data, bool dir)
{
    EnumPublicDirsData* enumPublicDirsData = (EnumPublicDirsData*)data;

    if(strcmp(name, enumPublicDirsData->name) == 0)
    {
        enumPublicDirsData->found = true;
        return false;
    }

    return true;
}

static void onEnumPublicDirsDone(void* data)
{
    EnumPublicDirsData* enumPublicDirsData = data;
    enumPublicDirsData->done(enumPublicDirsData->found, enumPublicDirsData->data);
    free(enumPublicDirsData->name);
    free(enumPublicDirsData);
}

bool fsIsDir(FileSystem* fs, const char* name)
{
    if (*name == '.') return false;
    if (isRoot(fs) && strcmp(name, PublicDir) == 0) return true;

#if defined(BAREMETALPI)
    dbg("fsIsDirSync %s\n", name);
    FILINFO s;

    FRESULT res = f_stat(name, &s);
    if (res != FR_OK) return false;

    return s.fattrib & AM_DIR;
#else
    const char* path = fsGetFilePath(fs, name);
    struct tic_stat_struct s;
    const FsString* pathString = utf8ToString(path);
    bool ret = tic_stat(pathString, &s) == 0 && S_ISDIR(s.st_mode);
    freeString(pathString);

    return ret;
#endif
}

void fsIsDirAsync(FileSystem* fs, const char* name, IsDirCallback callback, void* data)
{
    if(isPublicRoot(fs))
    {
        EnumPublicDirsData enumPublicDirsData = { strdup(name), false, callback, data};
        fsEnumFilesAsync(fs, onEnumPublicDirs, onEnumPublicDirsDone, OBJCOPY(enumPublicDirsData));
        return;
    }

    callback(fsIsDir(fs, name), data);
}

bool fsWriteFile(const char* name, const void* buffer, s32 size)
{
#if defined(BAREMETALPI)
    dbg("fsWriteFile %s\n", name);
    FIL File;
    FRESULT res = f_open (&File, name, FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK)
    {
        return false;
    }

    u32 written=0;
    
    res = f_write(&File, buffer, size, &written);
    
    f_close(&File);
    if (res != FR_OK)
    {
        return false;
    }
    if(written!=size)
    {   
        dbg("Write size diff %d %d", size, written);
        return false;
    }
    return true;
#else
    const FsString* pathString = utf8ToString(name);
    FILE* file = tic_fopen(pathString, _S("wb"));
    freeString(pathString);

    if(file)
    {
        fwrite(buffer, 1, size, file);
        fclose(file);

#if defined(__EMSCRIPTEN__)
        syncfs();
#endif

        return true;
    }

    return false;
#endif
}

void* fsReadFile(const char* path, s32* size)
{
#if defined(BAREMETALPI)
    dbg("fsReadFile %s\n", path);
    FILINFO fi;
    FRESULT res = f_stat(path, &fi);
    if(res!=FR_OK) return NULL;
    FIL file;
    res = f_open (&file, path, FA_READ | FA_OPEN_EXISTING);
    if(res!=FR_OK) return NULL;
    *size = fi.fsize; // size is in output!
    void* buffer = malloc(*size);
    UINT read = 0;
    res = f_read(&file, buffer, fi.fsize, &read);

    f_close(&file);
    if(read!=(*size)) return NULL;

    return buffer;

#else
    const FsString* pathString = utf8ToString(path);
    FILE* file = tic_fopen(pathString, _S("rb"));
    freeString(pathString);

    void* buffer = NULL;

    if(file)
    {

        fseek(file, 0, SEEK_END);
        *size = ftell(file);
        fseek(file, 0, SEEK_SET);

        if((buffer = malloc(*size)) && fread(buffer, *size, 1, file)) {}

        fclose(file);
    }

    return buffer;

#endif
}

static void makeDir(const char* name)
{
#if defined(BAREMETALPI)
    // TODO BAREMETALPI
    dbg("makeDir %s\n", name);

    char* path = strdup(name);
    if (path && *path) {                      // make sure result has at least
      if (path[strlen(path) - 1] == '/')    // one character
            path[strlen(path) - 1] = 0;
    }

    FRESULT res = f_mkdir(path);
    if(res != FR_OK)
    {
        dbg("Could not mkdir %s\n", name);
    }
    free(path);
#else
    const FsString* pathString = utf8ToString(name);
    tic_mkdir(pathString);
    freeString(pathString);

#if defined(__EMSCRIPTEN__)
    syncfs();
#endif
#endif
}

static void fsFullname(const char *path, char *fullname)
{
#if defined(BAREMETALPI) || defined(_3DS)
    dbg("fsFullname %s", path);
    // TODO BAREMETALPI
#else
#if defined(__TIC_WINDOWS__)
    static wchar_t wpath[TICNAME_MAX];

    const FsString* pathString = utf8ToString(path);
    GetFullPathNameW(pathString, sizeof(wpath), wpath, NULL);
    freeString(pathString);

    const char* res = stringToUtf8(wpath);

#else

    const char* res = realpath(path, NULL);

#endif

    strcpy(fullname, res);
    free((void*)res);
#endif
}

void fsFilename(const char *path, char* out)
{
    char full[TICNAME_MAX];
    fsFullname(path, full);

    char base[TICNAME_MAX];
    fsBasename(path, base);

    strcpy(out, full + strlen(base));
}

void fsBasename(const char *path, char* out)
{
#if defined(BAREMETALPI)
    // TODO BAREMETALPI
    dbg("fsBasename %s\n", path);
#define SEP "/"
#else

    char* result = NULL;

#if defined(__TIC_WINDOWS__)
#define SEP "\\"
#else
#define SEP "/"
#endif


    char full[TICNAME_MAX];
    fsFullname(path, full);

    struct tic_stat_struct s;

    const FsString* fullString = utf8ToString(full);
    s32 ret = tic_stat(fullString, &s);
    freeString(fullString);

    if(ret == 0)
    {
        result = full;

        if(S_ISREG(s.st_mode))
        {
            const char* ptr = result + strlen(result);

            while(ptr >= result)
            {
                if(*ptr == SEP[0])
                {
                    result[ptr-result] = '\0';
                    break;
                }

                ptr--;
            }
        }
    }

    if (result)
    {
        if (result[strlen(result) - 1] != SEP[0])
            strcat(result, SEP);

        strcpy(out, result);
    }
#endif
}

bool fsExists(const char* name)
{
#if defined(BAREMETALPI)
    dbg("fsExists %s\n", name);
    FILINFO s;

    FRESULT res = f_stat(name, &s);
    return res == FR_OK;
#else
    struct tic_stat_struct s;

    const FsString* pathString = utf8ToString(name);
    bool ret = tic_stat(pathString, &s) == 0;
    freeString(pathString);

    return ret;
#endif
}

bool fsExistsFile(FileSystem* fs, const char* name)
{
    return fsExists(fsGetFilePath(fs, name));
}

u64 fsMDate(const char* path)
{
#if defined(BAREMETALPI)
    dbg("fsMDate %s\n", path);
    // TODO BAREMETALPI
    return 0;
#else
    struct tic_stat_struct s;

    const FsString* pathString = utf8ToString(path);
    s32 ret = tic_stat(pathString, &s);
    freeString(pathString);

    if(ret == 0 && S_ISREG(s.st_mode))
    {
        return s.st_mtime;
    }

    return 0;
#endif
}

bool fsSaveFile(FileSystem* fs, const char* name, const void* data, s32 size, bool overwrite)
{
    if(!overwrite)
    {
        if(fsExistsFile(fs, name))
            return false;
    }

    return fsWriteFile(fsGetFilePath(fs, name), data, size);
}

bool fsSaveRootFile(FileSystem* fs, const char* name, const void* data, s32 size, bool overwrite)
{
    const char* path = fsGetRootFilePath(fs, name);

    if(!overwrite)
    {
        if(fsExists(path))
            return false;
    }

    return fsWriteFile(path, data, size);
}

typedef struct
{
    FileSystem* fs;
    LoadCallback done;
    void* data;
    char* cachePath;
} LoadFileByHashData;

static void fileByHashLoaded(const HttpGetData* netData)
{
    LoadFileByHashData* loadFileByHashData = netData->calldata;

    if (netData->type == HttpGetDone)
    {
        fsSaveRootFile(loadFileByHashData->fs, loadFileByHashData->cachePath, netData->done.data, netData->done.size, false);
        loadFileByHashData->done(netData->done.data, netData->done.size, loadFileByHashData->data);
    }

    switch (netData->type)
    {
    case HttpGetDone:
    case HttpGetError:

        free(loadFileByHashData->cachePath);
        free(loadFileByHashData);
        break;
    }
}

void fsLoadFileByHashAsync(FileSystem* fs, const char* hash, LoadCallback callback, void* data)
{
#if defined(BAREMETALPI)
    // TODO BAREMETALPI
    return NULL;
#else

    char cachePath[TICNAME_MAX];
    sprintf(cachePath, TIC_CACHE "%s.tic", hash);

    {
        s32 size = 0;
        void* buffer = fsLoadRootFile(fs, cachePath, &size);
        if (buffer)
        {
            callback(buffer, size, data);
            return;
        }
    }

    char path[TICNAME_MAX];
    sprintf(path, "/cart/%s/cart.tic", hash);

    LoadFileByHashData loadFileByHashData = { fs, callback, data, strdup(cachePath) };
    netGet(fs->net, path, fileByHashLoaded, OBJCOPY(loadFileByHashData));
#endif
}

void* fsLoadFile(FileSystem* fs, const char* name, s32* size)
{
#if defined(BAREMETALPI)
    dbg("fsLoadFile x %s\n", name);
    dbg("fs.dir %s\n", fs->dir);
    dbg("fs.work %s\n", fs->work);
    
    if(isPublic(fs))
    {
        dbg("Public ??\n");
        return NULL;
    }
    else
    {
        dbg("non public \n");
        const char* fp = fsGetFilePath(fs, name);
        dbg("loading: %s\n", fp);


        FILINFO fi;
        FRESULT res = f_stat(fp, &fi);
        dbg("fstat done %d \n", res);

        if(res!=FR_OK) 
        {
            dbg("NO F_STAT %d\n", res);
            return NULL;
        }
        FIL file;
        res = f_open (&file, fp, FA_READ | FA_OPEN_EXISTING);
        if(res!=FR_OK)
        {
            dbg("NO F_OPEN %d\n", res);
            return NULL;
        }
        dbg("BUFFERING %d\n", res);

        void* buffer = malloc(fi.fsize);
        dbg("BUFFERED %d\n", fi.fsize);

        UINT read = 0;
        res = f_read(&file, buffer, fi.fsize, &read);
        dbg("F_READ %d %ld\n", res, read);

        f_close(&file);
        if(read!=fi.fsize) 
        {
            dbg("NO F_READ %d \n", res);
            return NULL;
        }
        dbg("RETURNING!!\n");
        *size = fi.fsize;
        return buffer;
    }
    return NULL;
#else

    const FsString* pathString = utf8ToString(fsGetFilePath(fs, name));
    FILE* file = tic_fopen(pathString, _S("rb"));
    freeString(pathString);

    void* ptr = NULL;

    if(file)
    {
        fseek(file, 0, SEEK_END);
        *size = ftell(file);
        fseek(file, 0, SEEK_SET);

        u8* buffer = malloc(*size);

        if(buffer && fread(buffer, *size, 1, file)) ptr = buffer;

        fclose(file);
    }

    return ptr;     

#endif
}

void* fsLoadRootFile(FileSystem* fs, const char* name, s32* size)
{
    return fsReadFile(fsGetRootFilePath(fs, name), size);
}

void fsMakeDir(FileSystem* fs, const char* name)
{
    makeDir(fsGetFilePath(fs, name));
}

void fsOpenWorkingFolder(FileSystem* fs)
{
    const char* path = fsGetFilePath(fs, "");

    if(isPublic(fs))
        path = fs->dir;

    tic_sys_open_path(path);
}

FileSystem* createFileSystem(const char* path, Net* net)
{
    FileSystem* fs = (FileSystem*)calloc(1, sizeof(FileSystem));

    strcpy(fs->dir, path);

    if(path[strlen(path) - 1] != SEP[0])
        strcat(fs->dir, SEP);

    fs->net = net;

    return fs;
}
