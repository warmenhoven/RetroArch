//
//  JITSupport.m
//  RetroArchiOS
//
//  Created by Yoshi Sugawara on 9/25/21.
//  Copyright © 2021 RetroArch. All rights reserved.
//
//  Copied from UTMApp, original author: osy
//  

#import <Foundation/Foundation.h>

#import "JITSupport.h"

#include <dlfcn.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/getsect.h>
#include <pthread.h>
#include <dirent.h>

#include <sys/mman.h>
#include <signal.h>
#include <sys/ucontext.h>

#include <libretro.h>
#include <string/stdstring.h>
#include <file/file_path.h>
#include <retro_miscellaneous.h>
#include "../../verbosity.h"

extern int csops(pid_t pid, unsigned int ops, void * useraddr, size_t usersize);
extern boolean_t exc_server(mach_msg_header_t *, mach_msg_header_t *);
extern int ptrace(int request, pid_t pid, caddr_t addr, int data);

#define    CS_OPS_STATUS        0    /* return status */
#define CS_DEBUGGED 0x10000000  /* process is currently or has previously been debugged and allowed to run with invalid pages */

static bool jb_has_debugger_attached(void) {
    int flags;
    return !csops(getpid(), CS_OPS_STATUS, &flags, sizeof(flags)) && flags & CS_DEBUGGED;
}

#if !TARGET_OS_TV
#define PT_TRACE_ME     0       /* child declares it's being traced */
#define PT_SIGEXC       12      /* signals as exceptions for current_proc */

static void *exception_handler(void *argument) {
    mach_port_t port = *(mach_port_t *)argument;
    mach_msg_server(exc_server, 2048, port, 0);
    return NULL;
}

bool jb_enable_ptrace_hack(void) {
    bool debugged = jb_has_debugger_attached();
    
    // Thanks to this comment: https://news.ycombinator.com/item?id=18431524
    // We use this hack to allow mmap with PROT_EXEC (which usually requires the
    // dynamic-codesigning entitlement) by tricking the process into thinking
    // that Xcode is debugging it. We abuse the fact that JIT is needed to
    // debug the process.
    if (ptrace(PT_TRACE_ME, 0, NULL, 0) < 0) {
        return false;
    }
    
    // ptracing ourselves confuses the kernel and will cause bad things to
    // happen to the system (hangs…) if an exception or signal occurs. Setup
    // some "safety nets" so we can cause the process to exit in a somewhat sane
    // state. We only need to do this if the debugger isn't attached. (It'll do
    // this itself, and if we do it we'll interfere with its normal operation
    // anyways.)
    if (!debugged) {
        // First, ensure that signals are delivered as Mach software exceptions…
        ptrace(PT_SIGEXC, 0, NULL, 0);
        
        // …then ensure that this exception goes through our exception handler.
        // I think it's OK to just watch for EXC_SOFTWARE because the other
        // exceptions (e.g. EXC_BAD_ACCESS, EXC_BAD_INSTRUCTION, and friends)
        // will end up being delivered as signals anyways, and we can get them
        // once they're resent as a software exception.
        mach_port_t port = MACH_PORT_NULL;
        mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
        mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
        task_set_exception_ports(mach_task_self(), EXC_MASK_SOFTWARE, port, EXCEPTION_DEFAULT, THREAD_STATE_NONE);
        pthread_t thread;
        pthread_create(&thread, NULL, exception_handler, (void *)&port);
    } else {
        // JIT code frequently causes an EXC_BAD_ACCESS exception that lldb
        // cannot be convinced to ignore. Instead we can set up a nul handler
        // that effectively causes it to be ignored. Note that this sometimes
        // also hides actual crashes from the debugger.
        task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS, MACH_PORT_NULL, EXCEPTION_DEFAULT, THREAD_STATE_NONE);
    }
    
    return true;
}
#endif /* !TARGET_OS_TV */

#if !TARGET_OS_SIMULATOR
static bool device_has_txm(void)
{
   static bool has_txm = false;
   static dispatch_once_t once = 0;
   dispatch_once(&once, ^{
      if (@available(iOS 26, tvOS 26, *))
      {
         /* Check for TXM firmware on disk. Non-TXM devices (e.g. A10X)
            running iOS/tvOS 26 won't have this file. */
         NSString *bootUUID = nil;
         NSString *preboot = @"/System/Volumes/Preboot";
         NSError *error = nil;
         NSArray<NSString *> *items = [[NSFileManager defaultManager]
            contentsOfDirectoryAtPath:preboot error:&error];
         for (NSString *entry in items)
         {
            if (entry.length == 36)
            {
               bootUUID = [preboot stringByAppendingPathComponent:entry];
               break;
            }
         }
         if (bootUUID)
         {
            NSString *bootDir = [bootUUID stringByAppendingPathComponent:@"boot"];
            items = [[NSFileManager defaultManager]
               contentsOfDirectoryAtPath:bootDir error:&error];
            for (NSString *entry in items)
            {
               if (entry.length == 96)
               {
                  NSString *img = [[bootDir stringByAppendingPathComponent:entry]
                     stringByAppendingPathComponent:
                     @"usr/standalone/firmware/FUD/Ap,TrustedExecutionMonitor.img4"];
                  if (access(img.fileSystemRepresentation, F_OK) == 0)
                     has_txm = true;
                  break;
               }
            }
         }

         if (!has_txm)
         {
            /* Fallback: /private/preboot/<96>/usr/.../Ap,TrustedExecutionMonitor.img4 */
            items = [[NSFileManager defaultManager]
               contentsOfDirectoryAtPath:@"/private/preboot" error:&error];
            for (NSString *entry in items)
            {
               if (entry.length == 96)
               {
                  NSString *img = [[@"/private/preboot" stringByAppendingPathComponent:entry]
                     stringByAppendingPathComponent:
                     @"usr/standalone/firmware/FUD/Ap,TrustedExecutionMonitor.img4"];
                  if (access(img.fileSystemRepresentation, F_OK) == 0)
                     has_txm = true;
                  break;
               }
            }
         }
      }
   });
   return has_txm;
}

static bool requires_dual_map(void)
{
   if (@available(iOS 26, tvOS 26, *))
      return true;
   return false;
}

static volatile bool s_brk_trapped;
static void brk_trap_handler(int sig, siginfo_t *info, void *ctx)
{
   s_brk_trapped = true;
   ((ucontext_t *)ctx)->uc_mcontext->__ss.__pc += 4;
}

/* Ask the debugger to bless an R-X region so TXM allows execution.
 * Uses the universal.js protocol: brk #0xf00d with x16=1
 * (CMD_PREPARE_REGION). Installs a SIGTRAP handler so a missing
 * debugger doesn't crash — it just means the pages were NOT blessed.
 * Returns true if the debugger handled the brk. */
static bool bless_executable_region(void *ptr, size_t size)
{
   struct sigaction prev, act = {};
   act.sa_sigaction = brk_trap_handler;
   act.sa_flags     = SA_SIGINFO;
   sigemptyset(&act.sa_mask);
   sigaction(SIGTRAP, &act, &prev);
   s_brk_trapped = false;
   __asm__ volatile(
      "mov x0, %0\n"
      "mov x1, %1\n"
      "mov x16, #1\n"
      "brk #0xf00d"
      :: "r"(ptr), "r"(size)
      : "x0", "x1", "x16", "memory"
   );
   sigaction(SIGTRAP, &prev, NULL);
   return !s_brk_trapped;
}

/* Tell the debugger to detach. Uses the universal.js protocol:
 * brk #0xf00d with x16=0 (CMD_DETACH). After this call the debugger
 * disconnects and the process runs standalone. */
static void detach_debugger(void)
{
   struct sigaction prev, act = {};
   act.sa_sigaction = brk_trap_handler;
   act.sa_flags     = SA_SIGINFO;
   sigemptyset(&act.sa_mask);
   sigaction(SIGTRAP, &act, &prev);
   s_brk_trapped = false;
   __asm__ volatile(
      "mov x16, #0\n"
      "brk #0xf00d"
      ::: "x16", "memory"
   );
   sigaction(SIGTRAP, &prev, NULL);
}

/* Create a R-W mirror of an existing R-X region via vm_remap.
 * Returns the R-W pointer on success, NULL on failure. */
static void *create_rw_mirror(void *rx, size_t size)
{
   vm_address_t rw = 0;
   vm_prot_t cur = 0, max = 0;
   kern_return_t kr = vm_remap(mach_task_self(), &rw, size, 0,
                               VM_FLAGS_ANYWHERE, mach_task_self(),
                               (vm_address_t)rx, FALSE,
                               &cur, &max, VM_INHERIT_DEFAULT);
   if (kr != KERN_SUCCESS)
      return NULL;
   if (mprotect((void *)rw, size, PROT_READ | PROT_WRITE) != 0)
   {
      vm_deallocate(mach_task_self(), rw, size);
      return NULL;
   }
   return (void *)rw;
}
#endif /* !TARGET_OS_SIMULATOR */

/* ── Pre-allocated JIT memory pool ──────────────────────────────────
 * On iOS 26+ TXM devices the debugger must bless executable pages via
 * brk #0x69 before they can be executed. Rather than keeping the
 * debugger attached for the lifetime of the process, we allocate one
 * large pool at startup, bless it in a single brk call, and then the
 * debugger can detach. All subsequent exec_mem_alloc requests are
 * bump-allocated from this pre-blessed pool. */
#define EXEC_MEM_POOL_SIZE (544UL * 1024 * 1024)

static void    *s_pool_rx   = NULL;
static void    *s_pool_rw   = NULL;
static size_t   s_pool_size = 0;
static size_t   s_pool_used = 0;

bool exec_mem_pool_init(void)
{
   if (s_pool_rx)
      return true;

   if (!jb_has_debugger_attached())
      return false;

   if (!requires_dual_map())
      return false;

   size_t page = sysconf(_SC_PAGESIZE);
   size_t size = (EXEC_MEM_POOL_SIZE + page - 1) & ~(page - 1);

   void *rx = mmap(NULL, size, PROT_READ | PROT_EXEC,
                   MAP_ANON | MAP_PRIVATE, -1, 0);
   if (rx == MAP_FAILED)
      return false;

   /* TXM devices need the debugger to bless executable pages.
    * Non-TXM devices (e.g. A10X on tvOS 26) can mmap R-X freely
    * with CS_DEBUGGED — no blessing required. */
   if (device_has_txm() && !bless_executable_region(rx, size))
   {
      munmap(rx, size);
      return false;
   }

   void *rw = create_rw_mirror(rx, size);
   if (!rw)
   {
      munmap(rx, size);
      return false;
   }

   s_pool_rx   = rx;
   s_pool_rw   = rw;
   s_pool_size = size;
   s_pool_used = 0;
   RARCH_LOG("[JIT] Pool allocated: %zu MB (rx=%p rw=%p)\n",
             size / (1024 * 1024), rx, rw);

   /* All JIT memory is pre-blessed. Tell the debugger to detach —
    * we no longer need it and running without a debugger avoids
    * EXC_BAD_ACCESS interception overhead. */
   detach_debugger();
   RARCH_LOG("[JIT] Debugger detached\n");

   return true;
}

void exec_mem_pool_reset(void)
{
   s_pool_used = 0;
}

bool exec_mem_alloc(size_t *size, unsigned *mode, void **rx, void **rw)
{
#if TARGET_OS_SIMULATOR
   return false;
#else
   /* ── Pool path (iOS 26+ TXM) ── */
   if (s_pool_rx)
   {
      if (*size == 0)
      {
         *mode = RETRO_EXEC_MEM_MODE_DUAL_MAP;
         *size = s_pool_size - s_pool_used;
         *rx   = NULL;
         *rw   = NULL;
         return true;
      }

      size_t page = sysconf(_SC_PAGESIZE);
      *size = (*size + page - 1) & ~(page - 1);

      if (s_pool_used + *size > s_pool_size)
      {
         RARCH_ERR("[JIT] Pool exhausted: need %zu, have %zu\n",
                   *size, s_pool_size - s_pool_used);
         return false;
      }

      *mode = RETRO_EXEC_MEM_MODE_DUAL_MAP;
      *rx   = (uint8_t *)s_pool_rx + s_pool_used;
      *rw   = (uint8_t *)s_pool_rw + s_pool_used;
      s_pool_used += *size;
      return true;
   }

   /* ── Legacy per-allocation path (pre-iOS 26, jailbreak, etc.) ── */
   if (!jb_has_debugger_attached())
      return false;

   bool dual = requires_dual_map();

   if (*size == 0)
   {
      *mode = dual ? RETRO_EXEC_MEM_MODE_DUAL_MAP
                   : RETRO_EXEC_MEM_MODE_WX_TOGGLE;
      *rx   = NULL;
      *rw   = NULL;
      return true;
   }

   size_t page = sysconf(_SC_PAGESIZE);
   *size = (*size + page - 1) & ~(page - 1);

   if (dual)
   {
      void *ptr_rx = mmap(NULL, *size, PROT_READ | PROT_EXEC,
                          MAP_ANON | MAP_PRIVATE, -1, 0);
      if (ptr_rx == MAP_FAILED)
         return false;

      if (device_has_txm() && !bless_executable_region(ptr_rx, *size))
      {
         munmap(ptr_rx, *size);
         return false;
      }

      void *ptr_rw = create_rw_mirror(ptr_rx, *size);
      if (!ptr_rw)
      {
         munmap(ptr_rx, *size);
         return false;
      }

      *mode = RETRO_EXEC_MEM_MODE_DUAL_MAP;
      *rx   = ptr_rx;
      *rw   = ptr_rw;
      return true;
   }

   /* Pre-iOS 26: single mapping, core toggles W^X via mprotect */
   void *ptr = mmap(NULL, *size, PROT_READ | PROT_WRITE,
                    MAP_ANON | MAP_PRIVATE, -1, 0);
   if (ptr == MAP_FAILED)
      return false;
   *mode = RETRO_EXEC_MEM_MODE_WX_TOGGLE;
   *rx   = ptr;
   *rw   = ptr;
   return true;
#endif
}

void exec_mem_free(void *rx, void *rw, size_t size, bool dual)
{
   /* Pool allocations are freed in bulk via exec_mem_pool_reset */
   if (s_pool_rx)
      return;

   if (dual && rw && rw != rx)
      vm_deallocate(mach_task_self(), (vm_address_t)rw, size);
   if (rx)
      munmap(rx, size);
}

bool jit_available(void)
{
   /* Pool was allocated at startup — JIT is ready */
   if (s_pool_rx)
      return true;

   static bool canOpenApps = false;
   static dispatch_once_t appsOnce = 0;
   dispatch_once(&appsOnce, ^{
      DIR *apps = opendir("/Applications");
      if (apps)
      {
         closedir(apps);
         canOpenApps = true;
      }
   });

   static bool dylded = false;
   static dispatch_once_t dyldOnce = 0;
   dispatch_once(&dyldOnce, ^{
      int imageCount = _dyld_image_count();
      for (int i = 0; i < imageCount; i++)
      {
         if (string_is_equal("/usr/lib/pspawn_payload-stg2.dylib", _dyld_get_image_name(i)))
            dylded = true;
      }
   });

   static bool doped = false;
   static dispatch_once_t dopeOnce = 0;
   dispatch_once(&dopeOnce, ^{
      int64_t (*jbdswDebugMe)(void) = dlsym(RTLD_DEFAULT, "jbdswDebugMe");
      if (jbdswDebugMe)
      {
         int64_t ret = jbdswDebugMe();
         doped = (ret == 0);
      }
   });

   if (canOpenApps || dylded || doped)
      return true;

#if TARGET_OS_SIMULATOR
   return false;
#else
   if (!jb_has_debugger_attached())
      return false;

   if (requires_dual_map() && device_has_txm())
   {
      /* TXM device on iOS/tvOS 26: probe whether the debugger script
       * is handling brk #0x69.  Uses the safe SIGTRAP handler so a
       * missing script doesn't crash — it just means JIT won't work. */
      size_t page = sysconf(_SC_PAGESIZE);
      void *probe = mmap(NULL, page, PROT_READ | PROT_EXEC,
                         MAP_ANON | MAP_PRIVATE, -1, 0);
      if (probe == MAP_FAILED)
         return false;
      bool blessed = bless_executable_region(probe, page);
      munmap(probe, page);
      if (!blessed)
         return false;
   }
   return true;
#endif
}
