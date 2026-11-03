/* gpu.c - the CUDA driver and NVRTC, loaded by name. Only the few calls the tools use are bound,
 * with the signatures the driver has kept since CUDA 4, so any driver from the last decade does. */
#include "gpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef HMODULE Library;
static Library library_open(const char *path) {
    /* The compiler's own builtins sit beside it, and are only found when its folder is searched. */
    return LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}
static void *library_symbol(Library library, const char *name) {
    return (void *)(uintptr_t)GetProcAddress(library, name);
}
static const char *const DriverNames[]   = {"nvcuda.dll"};
static const char *const CompilerNames[] = {"nvrtc64_130_0.dll", "nvrtc64_120_0.dll",
                                            "nvrtc64_112_0.dll"};
#define PATH_SEPARATOR "\\"
#else
#include <dlfcn.h>
typedef void *Library;
static Library library_open(const char *path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
static void *library_symbol(Library library, const char *name) { return dlsym(library, name); }
static const char *const DriverNames[]   = {"libcuda.so.1", "libcuda.so"};
static const char *const CompilerNames[] = {"libnvrtc.so.13", "libnvrtc.so.12", "libnvrtc.so.11.2",
                                            "libnvrtc.so"};
#define PATH_SEPARATOR "/"
#endif

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

typedef int CUresult;
typedef void *CUcontext;
typedef void *CUmodule;
typedef void *CUfunction;
#define CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR 75
#define CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR 76

static struct {
    CUresult (*init)(unsigned);
    CUresult (*device_get)(int *, int);
    CUresult (*device_name)(char *, int, int);
    CUresult (*device_attribute)(int *, int, int);
    CUresult (*context_retain)(CUcontext *, int);
    CUresult (*context_set)(CUcontext);
    CUresult (*module_load)(CUmodule *, const void *);
    CUresult (*module_function)(CUfunction *, CUmodule, const char *);
    CUresult (*alloc)(GpuPtr *, size_t);
    CUresult (*host_alloc)(void **, size_t);
    CUresult (*upload)(GpuPtr, const void *, size_t);
    CUresult (*upload_async)(GpuPtr, const void *, size_t, void *);
    CUresult (*download)(void *, GpuPtr, size_t);
    CUresult (*zero)(GpuPtr, unsigned char, size_t);
    CUresult (*launch)(CUfunction, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                       unsigned, void *, void **, void **);
    CUresult (*wait)(void);
    CUresult (*error_string)(CUresult, const char **);
} Cu;

typedef int nvrtcResult;
typedef void *nvrtcProgram;

static struct {
    nvrtcResult (*create)(nvrtcProgram *, const char *, const char *, int, const char *const *,
                          const char *const *);
    nvrtcResult (*compile)(nvrtcProgram, int, const char *const *);
    nvrtcResult (*ptx_size)(nvrtcProgram, size_t *);
    nvrtcResult (*ptx)(nvrtcProgram, char *);
    nvrtcResult (*log_size)(nvrtcProgram, size_t *);
    nvrtcResult (*log)(nvrtcProgram, char *);
    nvrtcResult (*destroy)(nvrtcProgram *);
} Rtc;

static CUmodule Module;
static char Name[256];
static char Error[512];

static bool failed(CUresult result, const char *what) {
    if (result == 0) {
        return false;
    }
    const char *text = NULL;
    if (Cu.error_string) {
        Cu.error_string(result, &text);
    }
    snprintf(Error, sizeof(Error), "%s: %s (%d)", what, text ? text : "CUDA error", result);
    return true;
}

static Library open_any(const char *const *names, size_t count) {
    for (size_t i = 0; i < count; i++) {
        Library library = library_open(names[i]);
        if (library) {
            return library;
        }
    }
    /* The toolkit's own folder, which an installer sets but rarely puts on the path. */
    const char *root = getenv("CUDA_PATH");
    for (size_t i = 0; root && i < count; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s" PATH_SEPARATOR "bin" PATH_SEPARATOR "%s", root, names[i]);
        Library library = library_open(path);
        if (!library) {
            snprintf(path, sizeof(path), "%s" PATH_SEPARATOR "lib64" PATH_SEPARATOR "%s", root,
                     names[i]);
            library = library_open(path);
        }
        if (library) {
            return library;
        }
    }
    return NULL;
}

/* Binds `symbol` to `slot`, or names what was missing. */
#define BIND(library, slot, symbol)                                            \
    do {                                                                       \
        void *found = library_symbol(library, symbol);                         \
        if (!found) {                                                          \
            snprintf(why, why_size, "%s is missing from the library", symbol); \
            return false;                                                      \
        }                                                                      \
        memcpy(&(slot), &found, sizeof(found));                                \
    } while (0)

bool gpu_open(const char *source, char *why, size_t why_size) {
    Library driver = open_any(DriverNames, COUNT(DriverNames));
    if (!driver) {
        snprintf(why, why_size, "no CUDA driver");
        return false;
    }
    BIND(driver, Cu.init, "cuInit");
    BIND(driver, Cu.device_get, "cuDeviceGet");
    BIND(driver, Cu.device_name, "cuDeviceGetName");
    BIND(driver, Cu.device_attribute, "cuDeviceGetAttribute");
    BIND(driver, Cu.context_retain, "cuDevicePrimaryCtxRetain");
    BIND(driver, Cu.context_set, "cuCtxSetCurrent");
    BIND(driver, Cu.module_load, "cuModuleLoadData");
    BIND(driver, Cu.module_function, "cuModuleGetFunction");
    BIND(driver, Cu.alloc, "cuMemAlloc_v2");
    BIND(driver, Cu.host_alloc, "cuMemAllocHost_v2");
    BIND(driver, Cu.upload, "cuMemcpyHtoD_v2");
    BIND(driver, Cu.upload_async, "cuMemcpyHtoDAsync_v2");
    BIND(driver, Cu.download, "cuMemcpyDtoH_v2");
    BIND(driver, Cu.zero, "cuMemsetD8_v2");
    BIND(driver, Cu.launch, "cuLaunchKernel");
    BIND(driver, Cu.wait, "cuCtxSynchronize");
    BIND(driver, Cu.error_string, "cuGetErrorString");

    int device = 0, major = 0, minor = 0;
    CUcontext context;
    if (failed(Cu.init(0), "cuInit") || failed(Cu.device_get(&device, 0), "cuDeviceGet") ||
        failed(Cu.device_name(Name, sizeof(Name), device), "cuDeviceGetName") ||
        failed(Cu.device_attribute(&major, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR, device),
               "cuDeviceGetAttribute") ||
        failed(Cu.device_attribute(&minor, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, device),
               "cuDeviceGetAttribute") ||
        failed(Cu.context_retain(&context, device), "cuDevicePrimaryCtxRetain") ||
        failed(Cu.context_set(context), "cuCtxSetCurrent")) {
        snprintf(why, why_size, "%s", Error);
        return false;
    }

    Library compiler = open_any(CompilerNames, COUNT(CompilerNames));
    if (!compiler) {
        snprintf(why, why_size, "no NVRTC, the CUDA toolkit's runtime compiler");
        return false;
    }
    BIND(compiler, Rtc.create, "nvrtcCreateProgram");
    BIND(compiler, Rtc.compile, "nvrtcCompileProgram");
    BIND(compiler, Rtc.ptx_size, "nvrtcGetPTXSize");
    BIND(compiler, Rtc.ptx, "nvrtcGetPTX");
    BIND(compiler, Rtc.log_size, "nvrtcGetProgramLogSize");
    BIND(compiler, Rtc.log, "nvrtcGetProgramLog");
    BIND(compiler, Rtc.destroy, "nvrtcDestroyProgram");

    nvrtcProgram program;
    if (Rtc.create(&program, source, "kernels.cu", 0, NULL, NULL) != 0) {
        snprintf(why, why_size, "NVRTC would not take the source");
        return false;
    }
    char arch[64];
    snprintf(arch, sizeof(arch), "--gpu-architecture=compute_%d%d", major, minor);
    const char *options[] = {arch, "--std=c++17"};
    if (Rtc.compile(program, (int)COUNT(options), options) != 0) {
        size_t size = 0;
        Rtc.log_size(program, &size);
        char *log = malloc(size + 1);
        if (log) {
            Rtc.log(program, log);
            log[size] = '\0';
        }
        snprintf(why, why_size, "the kernels did not compile:\n%s", log ? log : "");
        free(log);
        Rtc.destroy(&program);
        return false;
    }
    size_t size = 0;
    Rtc.ptx_size(program, &size);
    char *ptx = malloc(size);
    if (!ptx) {
        snprintf(why, why_size, "out of memory");
        return false;
    }
    Rtc.ptx(program, ptx);
    Rtc.destroy(&program);
    const bool loaded = !failed(Cu.module_load(&Module, ptx), "cuModuleLoadData");
    free(ptx);
    if (!loaded) {
        snprintf(why, why_size, "%s", Error);
        return false;
    }
    return true;
}

const char *gpu_name(void) { return Name; }

void *gpu_function(const char *name) {
    CUfunction function = NULL;
    return failed(Cu.module_function(&function, Module, name), name) ? NULL : function;
}

bool gpu_alloc(GpuPtr *ptr, size_t bytes) { return !failed(Cu.alloc(ptr, bytes), "cuMemAlloc"); }

bool gpu_upload(GpuPtr to, const void *from, size_t bytes) {
    return !failed(Cu.upload(to, from, bytes), "cuMemcpyHtoD");
}

bool gpu_download(void *to, GpuPtr from, size_t bytes) {
    return !failed(Cu.download(to, from, bytes), "cuMemcpyDtoH");
}

bool gpu_zero(GpuPtr to, size_t bytes) { return !failed(Cu.zero(to, 0, bytes), "cuMemsetD8"); }

bool gpu_launch(void *function, unsigned blocks, unsigned threads, void **arguments) {
    return !failed(Cu.launch(function, blocks, 1, 1, threads, 1, 1, 0, NULL, arguments, NULL),
                   "cuLaunchKernel");
}

bool gpu_wait(void) { return !failed(Cu.wait(), "cuCtxSynchronize"); }

void *gpu_host_alloc(size_t bytes) {
    void *host = NULL;
    return failed(Cu.host_alloc(&host, bytes), "cuMemAllocHost") ? NULL : host;
}

bool gpu_upload_async(GpuPtr to, const void *from, size_t bytes) {
    return !failed(Cu.upload_async(to, from, bytes, NULL), "cuMemcpyHtoDAsync");
}

const char *gpu_error(void) { return Error; }
