/*
** 2004 May 22
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This file contains the VFS implementation for unix-like operating systems
** include Linux, MacOSX, *BSD, QNX, VxWorks, AIX, HPUX, and others.
**
** There are actually several different VFS implementations in this file.
** The differences are in the way that file locking is done.  The default
** implementation uses Posix Advisory Locks.  Alternative implementations
** use flock(), dot-files, various proprietary locking schemas, or simply
** skip locking all together.
**
** This source file is organized into divisions where the logic for various
** subfunctions is contained within the appropriate division.  PLEASE
** KEEP THE STRUCTURE OF THIS FILE INTACT.  New code should be placed
** in the correct division and should be clearly labeled.
**
** 本文件包括Unix类操作系统中VFS实现，包括Linux, MacOSX, *BSD, QNX
** VxWorks, AIX, HPUX, 和其他的。
** 实际上，在本文件中包含有VFS的多种实现，包括不同的文件
** 锁实现。默认的情况使用Posix咨询锁，可用选择的方式是使用
** flock(), 点文件，各种专有的锁定格式，或是跳过所有的锁。
** 这个源文件被组织为各种逻辑分区，子功能包括适当的划分，
** 必须要保持本文件的完整性，新代码必须要进行正确的划分，
** 进行明确的标记。
**
** The layout of divisions is as follows:
**
**   *  General-purpose declarations and utility functions.
**   *  Unique file ID logic used by VxWorks.
**   *  Various locking primitive implementations (all except proxy locking):
**      + for Posix Advisory Locks
**      + for no-op locks
**      + for dot-file locks
**      + for flock() locking
**      + for named semaphore locks (VxWorks only)
**      + for AFP filesystem locks (MacOSX only)
**   *  sqlite3_file methods not associated with locking.
**   *  Definitions of sqlite3_io_methods objects for all locking
**      methods plus "finder" functions for each locking method.
**   *  sqlite3_vfs method implementations.
**   *  Locking primitives for the proxy uber-locking-method. (MacOSX only)
**   *  Definitions of sqlite3_vfs objects for all locking methods
**      plus implementations of sqlite3_os_init() and sqlite3_os_end().
**
** 分区的全景如下：
**      通用的说明和实用功能
**      在VxWorks中使用的特殊文件ID逻辑
**	  各种锁独立的实现（除了代理锁定）：
**      + 咨询锁
**	   +无操作锁
**      +点文件锁
**      +flock()锁
**      +信号量锁（仅在VxWorks）
**      +AFP文件系统锁(仅在MacOSX)
**  sqlite3_file没有和锁关联的方法
**  sqlite3_io_methods对象为所有的锁对象添加了查找功能
**  锁文件的代理锁定方式(仅在MacOSX上)
**  在sqlite3_os_init()和sqlite3_os_ends*()
**  对于sqlite3_vfs对象所有锁定方式的实现方式，"
*/
#include "sqliteInt.h"
#if SQLITE_OS_UNIX              /* This file is used on unix only */

/*
** There are various methods for file locking used for concurrency
** control:
**
**   1. POSIX locking (the default),
**   2. No locking,
**   3. Dot-file locking,
**   4. flock() locking,
**   5. AFP locking (OSX only),
**   6. Named POSIX semaphores (VXWorks only),
**   7. proxy locking. (OSX only)
**
** Styles 4, 5, and 7 are only available of SQLITE_ENABLE_LOCKING_STYLE
** is defined to 1.  The SQLITE_ENABLE_LOCKING_STYLE also enables automatic
** selection of the appropriate locking style based on the filesystem
** where the database is located.  
** 
** 在进行文件锁定管理中有许多方法；
** 1. 默认情况下为POSIX 锁定
** 2. 没有文件锁
** 3. dot文件链接，
** 4. flock()文件
** 5. AFP文件锁定（仅用于OSX）
** 6. POSIX信号量(只有VXWork)
** 7, 代理锁定，（进入OSX系统）
** 
** 方法字符类型4，5，7都只在SQLITE_ENABLE_LockIngPStyle中设置为1
** SQLITE_ENABLE_LOCKING_STYLE也会根据数据库所在的位置自动选择
** 恰当的锁形式
**
***/
#if !defined(SQLITE_ENABLE_LOCKING_STYLE)
#  if defined(__APPLE__)
#    define SQLITE_ENABLE_LOCKING_STYLE 1
#  else
#    define SQLITE_ENABLE_LOCKING_STYLE 0
#  endif
#endif

/*
** Define the OS_VXWORKS pre-processor macro to 1 if building on 
** vxworks, or 0 otherwise.
**
** 如果工作在vxworks上，定义OS_VXWORKS预定义宏为1，否则为0
*/
#ifndef OS_VXWORKS
#  if defined(__RTP__) || defined(_WRS_KERNEL)
#    define OS_VXWORKS 1
#  else
#    define OS_VXWORKS 0
#  endif
#endif

/*
** These #defines should enable >2GB file support on Posix if the
** underlying operating system supports it.  If the OS lacks
** large file support, these should be no-ops.
**
** Large file support can be disabled using the -DSQLITE_DISABLE_LFS switch
** on the compiler command line.  This is necessary if you are compiling
** on a recent machine (ex: RedHat 7.2) but you want your code to work
** on an older machine (ex: RedHat 6.0).  If you compile on RedHat 7.2
** without this option, LFS is enable.  But LFS does not exist in the kernel
** in RedHat 6.0, so the code won't work.  Hence, for maximum binary
** portability you should omit LFS.
**
** The previous paragraph was written in 2005.  (This paragraph is written
** on 2008-11-28.) These days, all Linux kernels support large files, so
** you should probably leave LFS enabled.  But some embedded platforms might
** lack LFS in which case the SQLITE_DISABLE_LFS macro might still be useful.
**
** 这些定义使得需要支持大于2GB内存的POSIX文件可以
** 在操作系统层支持。如果操作系统层缺少这种支持，
** 那么就不应该有这些操作。
**
** 在编译器命令行中应该使用-DSQLITE_DISABLE_LFS开关禁止使用大文件
** 如果你在一些最近的机器上编译但是希望在旧一点的版本上工作
** 如果你在RedHat7.2上没有选择这个选项，默认开启LFS，但是由于在
** RedHat6.0上面没有LFS，这个代码就不会工作。因此，对于最大版本的
** 可携带二进制文件应该省略LFS选项
**
** 上面一段是在2005时写的（本段是在2008-11-28）最近，所有的Linux内核
** 都支持大文件，所以可以忽略LFS选项，但是对于一些嵌入式平台
** 可能工作在缺少LFS的环境，SQLITE_DISABLE_LFS宏仍然是有效的
*/
#ifndef SQLITE_DISABLE_LFS
# define _LARGE_FILE       1
# ifndef _FILE_OFFSET_BITS
#   define _FILE_OFFSET_BITS 64
# endif
# define _LARGEFILE_SOURCE 1
#endif

/*
** standard include files.
*/
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#ifndef SQLITE_OMIT_WAL
#include <sys/mman.h>
#endif


#if SQLITE_ENABLE_LOCKING_STYLE
# include <sys/ioctl.h>
# if OS_VXWORKS
#  include <semaphore.h>
#  include <limits.h>
# else
#  include <sys/file.h>
#  include <sys/param.h>
# endif
#endif /* SQLITE_ENABLE_LOCKING_STYLE  SQLITE允许锁形式*/

#if defined(__APPLE__) || (SQLITE_ENABLE_LOCKING_STYLE && !OS_VXWORKS)
# include <sys/mount.h>
#endif

#ifdef HAVE_UTIME
# include <utime.h>
#endif

/*
** Allowed values of unixFile.fsFlags 运行unixFile.fsFlags
*/
#define SQLITE_FSFLAGS_IS_MSDOS     0x1

/*
** If we are to be thread-safe, include the pthreads header and define
** the SQLITE_UNIX_THREADS macro.
**
** 如果我们需要达到进程安全，需要包括pthreads头文件
** 且定义SQLITE_UNIX_THREADS宏
*/
#if SQLITE_THREADSAFE
# include <pthread.h>
# define SQLITE_UNIX_THREADS 1
#endif

/*
** Default permissions when creating a new file
**
** 在创建新文件时的权限
*/
#ifndef SQLITE_DEFAULT_FILE_PERMISSIONS
# define SQLITE_DEFAULT_FILE_PERMISSIONS 0644
#endif

/*
** Default permissions when creating auto proxy dir
**
** 创建自动文件时的默认权限
*/
#ifndef SQLITE_DEFAULT_PROXYDIR_PERMISSIONS
# define SQLITE_DEFAULT_PROXYDIR_PERMISSIONS 0755
#endif

/*
** Maximum supported path-length.
**
** 对于路径长度的最大值
*/
#define MAX_PATHNAME 512

/*
** Only set the lastErrno if the error code is a real error and not 
** a normal expected return code of SQLITE_BUSY or SQLITE_OK
** 
** 如果错误代码是一个实际中的错误同时不正常返回
** SQLITE_BUSY或SQLITE_OK,仅设置lastErrno
*/
#define IS_LOCK_ERROR(x)  ((x != SQLITE_OK) && (x != SQLITE_BUSY))

/* Forward references */
typedef struct unixShm unixShm;               /* Connection shared memory */
typedef struct unixShmNode unixShmNode;       /* Shared memory instance */
typedef struct unixInodeInfo unixInodeInfo;   /* An i-node */
typedef struct UnixUnusedFd UnixUnusedFd;     /* An unused file descriptor */

/*
** Sometimes, after a file handle is closed by SQLite, the file descriptor
** cannot be closed immediately. In these cases, instances of the following
** structure are used to store the file descriptor while waiting for an
** opportunity to either close or reuse it.
**
** 有时，在SQLite关闭一个文件句柄之后，文件描述符并不能被
** 即刻关闭，在这些情况下，下列对象的实例都用于存储文件
** 描述，在等待关闭或者重用的机会
*/
struct UnixUnusedFd {
  int fd;                   /* File descriptor to close */
  int flags;                /* Flags this file descriptor was opened with */
  UnixUnusedFd *pNext;      /* Next unused file descriptor on same file */
};

/*
** The unixFile structure is subclass of sqlite3_file specific to the unix
** VFS implementations.
**
** uinxFile结构是unix VFS在sqlite3_file中的具体实现
*/
typedef struct unixFile unixFile;
struct unixFile {
  sqlite3_io_methods const *pMethod;  /* Always the first entry */
  sqlite3_vfs *pVfs;                  /* The VFS that created this unixFile */
  unixInodeInfo *pInode;              /* Info about locks on this inode */
  int h;                              /* The file descriptor */
  unsigned char eFileLock;            /* The type of lock held on this fd */
  unsigned short int ctrlFlags;       /* Behavioral bits.  UNIXFILE_* flags */
  int lastErrno;                      /* The unix errno from last I/O error */
  void *lockingContext;               /* Locking style specific state */
  UnixUnusedFd *pUnused;              /* Pre-allocated UnixUnusedFd */
  const char *zPath;                  /* Name of the file */
  unixShm *pShm;                      /* Shared memory segment information */
  int szChunk;                        /* Configured by FCNTL_CHUNK_SIZE */
#if SQLITE_ENABLE_LOCKING_STYLE
  int openFlags;                      /* The flags specified at open() */
#endif
#if SQLITE_ENABLE_LOCKING_STYLE || defined(__APPLE__)
  unsigned fsFlags;                   /* cached details from statfs() */
#endif
#if OS_VXWORKS
  struct vxworksFileId *pId;          /* Unique file ID */
#endif
#ifdef SQLITE_DEBUG
  /* The next group of variables are used to track whether or not the
  ** transaction counter in bytes 24-27 of database files are updated
  ** whenever any part of the database changes.  An assertion fault will
  ** occur if a file is updated without also updating the transaction
  ** counter.  This test is made to avoid new problems similar to the
  ** one described by ticket #3584. 
  ** 
  ** 接下来的一组变量用于追踪数据库文件中24-27字节处
  ** 是否进行了程序追踪，仅发生文件更新而无事务更新
  ** 就会出现错误。这个测试是用于避免#3584中出现的那种情况
  */
  unsigned char transCntrChng;   /* True if the transaction counter changed */
  unsigned char dbUpdate;        /* True if any part of database file changed */
  unsigned char inNormalWrite;   /* True if in a normal write operation */
#endif
#ifdef SQLITE_TEST
  /* In test mode, increase the size of this structure a bit so that 
  ** it is larger than the struct CrashFile defined in test6.c.
  **
  ** 在测试模型中，这个结构增加一个位都会让在test6.c中定义的
  ** CrashFile 结构变得更大
  */
  char aPadding[32];
#endif
};

/*
** Allowed values for the unixFile.ctrlFlags bitmask:
**
** 对unixFile.ctrlFlags进行设置掩码
*/
#define UNIXFILE_EXCL        0x01     /* Connections from one process only */
#define UNIXFILE_RDONLY      0x02     /* Connection is read only */
#define UNIXFILE_PERSIST_WAL 0x04     /* Persistent WAL mode */
#ifndef SQLITE_DISABLE_DIRSYNC
# define UNIXFILE_DIRSYNC    0x08     /* Directory sync needed */
#else
# define UNIXFILE_DIRSYNC    0x00
#endif
#define UNIXFILE_PSOW        0x10     /* SQLITE_IOCAP_POWERSAFE_OVERWRITE */
#define UNIXFILE_DELETE      0x20     /* Delete on close */
#define UNIXFILE_URI         0x40     /* Filename might have query parameters */
#define UNIXFILE_NOLOCK      0x80     /* Do no file locking */

/*
** Include code that is common to all os_*.c files
**
** 包含所有的os_*.c文件
*/
#include "os_common.h"

/*
** Define various macros that are missing from some systems.
**
** 定义在一些系统中缺少的宏
*/
#ifndef O_LARGEFILE
# define O_LARGEFILE 0
#endif
#ifdef SQLITE_DISABLE_LFS
# undef O_LARGEFILE
# define O_LARGEFILE 0
#endif
#ifndef O_NOFOLLOW
# define O_NOFOLLOW 0
#endif
#ifndef O_BINARY
# define O_BINARY 0
#endif

/*
** The threadid macro resolves to the thread-id or to 0.  Used for
** testing and debugging only.
**
** 线程宏解决线程id或0，仅用于测试或者是调试
*/
#if SQLITE_THREADSAFE
#define threadid pthread_self()
#else
#define threadid 0
#endif

/*
** Different Unix systems declare open() in different ways.  Same use
** open(const char*,int,mode_t).  Others use open(const char*,int,...).
** The difference is important when using a pointer to the function.
**
** The safest way to deal with the problem is to always use this wrapper
** which always has the same well-defined interface.
**
** 不同的Unix系统在定义open()时采用不同的方式。有些使用
** open(const char*, int, mode_t), 也有使用open(const char*, int,...)
** 主要的区别在于使用指向函数的指针。
** 最安全的方式去解决问题是使用包装器，接口定义规范.
*/
static int posixOpen(const char *zFile, int flags, int mode){
  return open(zFile, flags, mode);
}

/*
** On some systems, calls to fchown() will trigger a message in a security
** log if they come from non-root processes.  So avoid calling fchown() if
** we are not running as root.
** 
** 在一些系统中，请求fchown()会触发一个安全日志信息
** 如果来自非主进程。所以不是作为root运行时不要调
** 用fchown()函数
*/
static int posixFchown(int fd, uid_t uid, gid_t gid){
  return geteuid() ? 0 : fchown(fd,uid,gid);
}

/* Forward reference */
static int openDirectory(const char*, int*);

/*
** Many system calls are accessed through pointer-to-functions so that
** they may be overridden at runtime to facilitate fault injection during
** testing and sandboxing.  The following array holds the names and pointers
** to all overrideable system calls.
**
** 许多系统调用通过函数指针从而避免重写，在测试或者沙盒中
** 出现注射故障。接下来的数组用于保存名称和被忽略的所有系
** 调用的函数指针。
*/
static struct unix_syscall {
  const char *zName;            /* Name of the sytem call */
  sqlite3_syscall_ptr pCurrent; /* Current value of the system call */
  sqlite3_syscall_ptr pDefault; /* Default value */
} aSyscall[] = {
  { "open",         (sqlite3_syscall_ptr)posixOpen,  0  },
#define osOpen      ((int(*)(const char*,int,int))aSyscall[0].pCurrent)

  { "close",        (sqlite3_syscall_ptr)close,      0  },
#define osClose     ((int(*)(int))aSyscall[1].pCurrent)

  { "access",       (sqlite3_syscall_ptr)access,     0  },
#define osAccess    ((int(*)(const char*,int))aSyscall[2].pCurrent)

  { "getcwd",       (sqlite3_syscall_ptr)getcwd,     0  },
#define osGetcwd    ((char*(*)(char*,size_t))aSyscall[3].pCurrent)

  { "stat",         (sqlite3_syscall_ptr)stat,       0  },
#define osStat      ((int(*)(const char*,struct stat*))aSyscall[4].pCurrent)

/*
** The DJGPP compiler environment looks mostly like Unix, but it
** lacks the fcntl() system call.  So redefine fcntl() to be something
** that always succeeds.  This means that locking does not occur under
** DJGPP.  But it is DOS - what did you expect?
**
** DJGPP编译环境看起来最像Unix,但它缺少fcntl()系统调用，所以重定义
** fcntl()总是会成功的。这就意味着在DJGPP中不会出现锁，但是它实际
** 是DOS，你以为呢？
*/
#ifdef __DJGPP__
  { "fstat",        0,                 0  },
#define osFstat(a,b,c)    0
#else     
  { "fstat",        (sqlite3_syscall_ptr)fstat,      0  },
#define osFstat     ((int(*)(int,struct stat*))aSyscall[5].pCurrent)
#endif

  { "ftruncate",    (sqlite3_syscall_ptr)ftruncate,  0  },
#define osFtruncate ((int(*)(int,off_t))aSyscall[6].pCurrent)

  { "fcntl",        (sqlite3_syscall_ptr)fcntl,      0  },
#define osFcntl     ((int(*)(int,int,...))aSyscall[7].pCurrent)

  { "read",         (sqlite3_syscall_ptr)read,       0  },
#define osRead      ((ssize_t(*)(int,void*,size_t))aSyscall[8].pCurrent)

#if defined(USE_PREAD) || SQLITE_ENABLE_LOCKING_STYLE
  { "pread",        (sqlite3_syscall_ptr)pread,      0  },
#else
  { "pread",        (sqlite3_syscall_ptr)0,          0  },
#endif
#define osPread     ((ssize_t(*)(int,void*,size_t,off_t))aSyscall[9].pCurrent)

#if defined(USE_PREAD64)
  { "pread64",      (sqlite3_syscall_ptr)pread64,    0  },
#else
  { "pread64",      (sqlite3_syscall_ptr)0,          0  },
#endif
#define osPread64   ((ssize_t(*)(int,void*,size_t,off_t))aSyscall[10].pCurrent)

  { "write",        (sqlite3_syscall_ptr)write,      0  },
#define osWrite     ((ssize_t(*)(int,const void*,size_t))aSyscall[11].pCurrent)

#if defined(USE_PREAD) || SQLITE_ENABLE_LOCKING_STYLE
  { "pwrite",       (sqlite3_syscall_ptr)pwrite,     0  },
#else
  { "pwrite",       (sqlite3_syscall_ptr)0,          0  },
#endif
#define osPwrite    ((ssize_t(*)(int,const void*,size_t,off_t))\
                    aSyscall[12].pCurrent)

#if defined(USE_PREAD64)
  { "pwrite64",     (sqlite3_syscall_ptr)pwrite64,   0  },
#else
  { "pwrite64",     (sqlite3_syscall_ptr)0,          0  },
#endif
#define osPwrite64  ((ssize_t(*)(int,const void*,size_t,off_t))\
                    aSyscall[13].pCurrent)

#if SQLITE_ENABLE_LOCKING_STYLE
  { "fchmod",       (sqlite3_syscall_ptr)fchmod,     0  },
#else
  { "fchmod",       (sqlite3_syscall_ptr)0,          0  },
#endif
#define osFchmod    ((int(*)(int,mode_t))aSyscall[14].pCurrent)

#if defined(HAVE_POSIX_FALLOCATE) && HAVE_POSIX_FALLOCATE
  { "fallocate",    (sqlite3_syscall_ptr)posix_fallocate,  0 },
#else
  { "fallocate",    (sqlite3_syscall_ptr)0,                0 },
#endif
#define osFallocate ((int(*)(int,off_t,off_t))aSyscall[15].pCurrent)

  { "unlink",       (sqlite3_syscall_ptr)unlink,           0 },
#define osUnlink    ((int(*)(const char*))aSyscall[16].pCurrent)

  { "openDirectory",    (sqlite3_syscall_ptr)openDirectory,      0 },
#define osOpenDirectory ((int(*)(const char*,int*))aSyscall[17].pCurrent)

  { "mkdir",        (sqlite3_syscall_ptr)mkdir,           0 },
#define osMkdir     ((int(*)(const char*,mode_t))aSyscall[18].pCurrent)

  { "rmdir",        (sqlite3_syscall_ptr)rmdir,           0 },
#define osRmdir     ((int(*)(const char*))aSyscall[19].pCurrent)

  { "fchown",       (sqlite3_syscall_ptr)posixFchown,     0 },
#define osFchown    ((int(*)(int,uid_t,gid_t))aSyscall[20].pCurrent)

  { "umask",        (sqlite3_syscall_ptr)umask,           0 },
#define osUmask     ((mode_t(*)(mode_t))aSyscall[21].pCurrent)

}; /* End of the overrideable system calls */

/*
** This is the xSetSystemCall() method of sqlite3_vfs for all of the
** "unix" VFSes.  Return SQLITE_OK opon successfully updating the
** system call pointer, or SQLITE_NOTFOUND if there is no configurable
** system call named zName.
**
** 这是sqlite3_vfs 为所有"unix"VFSes准备的xSetSystemCall方法
** 在更新系统调用指针成功返回SQLITE_OK，如果没
** 有配置zName则返回SQLITE_NOTFOUND
*/
static int unixSetSystemCall(
  sqlite3_vfs *pNotUsed,        /* The VFS pointer.  Not used */
  const char *zName,            /* Name of system call to override */
  sqlite3_syscall_ptr pNewFunc  /* Pointer to new system call value */
){
  unsigned int i;
  int rc = SQLITE_NOTFOUND;

  UNUSED_PARAMETER(pNotUsed);
  if( zName==0 ){
    /* If no zName is given, restore all system calls to their default
    ** settings and return NULL
    ** 
    ** 如果没有给定zName，重新设置所有系统调用并返回空
    */
    rc = SQLITE_OK;
    for(i=0; i<sizeof(aSyscall)/sizeof(aSyscall[0]); i++){
      if( aSyscall[i].pDefault ){
        aSyscall[i].pCurrent = aSyscall[i].pDefault;
      }
    }
  }else{
    /* If zName is specified, operate on only the one system call
    ** specified.
    **
    ** 如果zName给定，那么就对特定的系统调用进行操作
    */
    for(i=0; i<sizeof(aSyscall)/sizeof(aSyscall[0]); i++){
      if( strcmp(zName, aSyscall[i].zName)==0 ){
        if( aSyscall[i].pDefault==0 ){
          aSyscall[i].pDefault = aSyscall[i].pCurrent;
        }
        rc = SQLITE_OK;
        if( pNewFunc==0 ) pNewFunc = aSyscall[i].pDefault;
        aSyscall[i].pCurrent = pNewFunc;
        break;
      }
    }
  }
  return rc;
}

/*
** Return the value of a system call.  Return NULL if zName is not a
** recognized system call name.  NULL is also returned if the system call
** is currently undefined.
**
** 返回系统调用的值，如果zName不是默认的系统调用名称
** 就返回空，在zName未定义的情况下也返回空。
*/
static sqlite3_syscall_ptr unixGetSystemCall(
  sqlite3_vfs *pNotUsed,
  const char *zName
){
  unsigned int i;

  UNUSED_PARAMETER(pNotUsed);
  for(i=0; i<sizeof(aSyscall)/sizeof(aSyscall[0]); i++){
    if( strcmp(zName, aSyscall[i].zName)==0 ) return aSyscall[i].pCurrent;
  }
  return 0;
}

/*
** Return the name of the first system call after zName.  If zName==NULL
** then return the name of the first system call.  Return NULL if zName
** is the last system call or if zName is not the name of a valid
** system call.
** 
** 返回zName之后的系统调用名称，如果zName==NULL就返回第一个
** 系统调用的名称，如果zName为空或者不是一个有效的系统调用
** 则返回最后一个系统调用名称
*/
static const char *unixNextSystemCall(sqlite3_vfs *p, const char *zName){
  int i = -1;

  UNUSED_PARAMETER(p);
  if( zName ){
    for(i=0; i<ArraySize(aSyscall)-1; i++){
      if( strcmp(zName, aSyscall[i].zName)==0 ) break;
    }
  }
  for(i++; i<ArraySize(aSyscall); i++){
    if( aSyscall[i].pCurrent!=0 ) return aSyscall[i].zName;
  }
  return 0;
}

/*
** Invoke open().  Do so multiple times, until it either succeeds or
** fails for some reason other than EINTR.
**
** If the file creation mode "m" is 0 then set it to the default for
** SQLite.  The default is SQLITE_DEFAULT_FILE_PERMISSIONS (normally
** 0644) as modified by the system umask.  If m is not 0, then
** make the file creation mode be exactly m ignoring the umask.
**
** The m parameter will be non-zero only when creating -wal, -journal,
** and -shm files.  We want those files to have *exactly* the same
** permissions as their original database, unadulterated by the umask.
** In that way, if a database file is -rw-rw-rw or -rw-rw-r-, and a
** transaction crashes and leaves behind hot journals, then any
** process that is able to write to the database will also be able to
** recover the hot journals.
**
** 调用open(),多次进行，直到既不是成功或失败，也不同于EINTR。
** 如果文件创建模式中"m"为0然后设置为默认值。默认的SQLITE_
** DEFAULT_FILE_PERMISSIONS(一般为0644)作为系统掩码。如果m不是0.则将
** 文件创建模式为-wal, -journal, -shm文件时，相关参数可以为0，我们
** 希望创建的数据库文件拥有和原始文件相同的权限。如果数据库
** 文件权限为-rw-rw-rw或-rw-rw-r-，同是一个原子性操作失败的时候
** 导致热日志，然后其他可以对本数据库进行写操作的进程就可以
** 修复这个热日志。
*/
static int robust_open(const char *z, int f, mode_t m){
  int fd;
  mode_t m2;
  mode_t origM = 0;
  if( m==0 ){
    m2 = SQLITE_DEFAULT_FILE_PERMISSIONS;
  }else{
    m2 = m;
    origM = osUmask(0);
  }
  do{
#if defined(O_CLOEXEC)
    fd = osOpen(z,f|O_CLOEXEC,m2);
#else
    fd = osOpen(z,f,m2);
#endif
  }while( fd<0 && errno==EINTR );
  if( m ){
    osUmask(origM);
  }
#if defined(FD_CLOEXEC) && (!defined(O_CLOEXEC) || O_CLOEXEC==0)
  if( fd>=0 ) osFcntl(fd, F_SETFD, osFcntl(fd, F_GETFD, 0) | FD_CLOEXEC);
#endif
  return fd;
}

/*
** Helper functions to obtain and relinquish the global mutex. The
** global mutex is used to protect the unixInodeInfo and
** vxworksFileId objects used by this file, all of which may be 
** shared by multiple threads.
**
** Function unixMutexHeld() is used to assert() that the global mutex 
** is held when required. This function is only used as part of assert() 
** statements. e.g.
**
**   unixEnterMutex()
**     assert( unixMutexHeld() );
**   unixEnterLeave()
**
** 帮助函数可以获得和释放全局互斥锁，可以用于保护
** 本文件中使用的unixInodeInfo和vxworksFileId对象，这些都可以
** 由多进程共享。
**
** 功能unixMutexHeld()使用assert()获得全局互斥锁，只能使用部分
** assert()语句。
**
**	unixEnterMutex()
**	   assert(unixMutexHeld());
**	unixEnterLeave()
*/
static void unixEnterMutex(void){
  sqlite3_mutex_enter(sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_MASTER));
}
static void unixLeaveMutex(void){
  sqlite3_mutex_leave(sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_MASTER));
}
#ifdef SQLITE_DEBUG
static int unixMutexHeld(void) {
  return sqlite3_mutex_held(sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_MASTER));
}
#endif


#if defined(SQLITE_TEST) && defined(SQLITE_DEBUG)
/*
** Helper function for printing out trace information from debugging
** binaries. This returns the string represetation of the supplied
** integer lock-type.
**
** 帮助函数用于从调试数据中打印跟踪信息。
** 返回值提供的字符串表示为整数锁类型
*/
static const char *azFileLock(int eFileLock){
  switch( eFileLock ){
    case NO_LOCK: return "NONE";
    case SHARED_LOCK: return "SHARED";
    case RESERVED_LOCK: return "RESERVED";
    case PENDING_LOCK: return "PENDING";
    case EXCLUSIVE_LOCK: return "EXCLUSIVE";
  }
  return "ERROR";
}
#endif

#ifdef SQLITE_LOCK_TRACE
/*
** Print out information about all locking operations.
**
** This routine is used for troubleshooting locks on multithreaded
** platforms.  Enable by compiling with the -DSQLITE_LOCK_TRACE
** command-line option on the compiler.  This code is normally
** turned off.
** 
** 打印所有的锁信息
** 本例程主要用于解决多线程锁问题，在编译器
** 命令行选项上使用-DSQLITE_LOCK_TRACE，常规情况下关闭
*/
static int lockTrace(int fd, int op, struct flock *p){
  char *zOpName, *zType;
  int s;
  int savedErrno;
  if( op==F_GETLK ){
    zOpName = "GETLK";
  }else if( op==F_SETLK ){
    zOpName = "SETLK";
  }else{
    s = osFcntl(fd, op, p);
    sqlite3DebugPrintf("fcntl unknown %d %d %d\n", fd, op, s);
    return s;
  }
  if( p->l_type==F_RDLCK ){
    zType = "RDLCK";
  }else if( p->l_type==F_WRLCK ){
    zType = "WRLCK";
  }else if( p->l_type==F_UNLCK ){
    zType = "UNLCK";
  }else{
    assert( 0 );
  }
  assert( p->l_whence==SEEK_SET );
  s = osFcntl(fd, op, p);
  savedErrno = errno;
  sqlite3DebugPrintf("fcntl %d %d %s %s %d %d %d %d\n",
     threadid, fd, zOpName, zType, (int)p->l_start, (int)p->l_len,
     (int)p->l_pid, s);
  if( s==(-1) && op==F_SETLK && (p->l_type==F_RDLCK || p->l_type==F_WRLCK) ){
    struct flock l2;
    l2 = *p;
    osFcntl(fd, F_GETLK, &l2);
    if( l2.l_type==F_RDLCK ){
      zType = "RDLCK";
    }else if( l2.l_type==F_WRLCK ){
      zType = "WRLCK";
    }else if( l2.l_type==F_UNLCK ){
      zType = "UNLCK";
    }else{
      assert( 0 );
    }
    sqlite3DebugPrintf("fcntl-failure-reason: %s %d %d %d\n",
       zType, (int)l2.l_start, (int)l2.l_len, (int)l2.l_pid);
  }
  errno = savedErrno;
  return s;
}
#undef osFcntl
#define osFcntl lockTrace
#endif /* SQLITE_LOCK_TRACE */

/*
** Retry ftruncate() calls that fail due to EINTR 
**
** 由于EINTR导致ftruncate()函数失败需要重试
*/
static int robust_ftruncate(int h, sqlite3_int64 sz){
  int rc;
  do{ rc = osFtruncate(h,sz); }while( rc<0 && errno==EINTR );
  return rc;
}

/*
** This routine translates a standard POSIX errno code into something
** useful to the clients of the sqlite3 functions.  Specifically, it is
** intended to translate a variety of "try again" errors into SQLITE_BUSY
** and a variety of "please close the file descriptor NOW" errors into 
** SQLITE_IOERR
** 
** Errors during initialization of locks, or file system support for locks,
** should handle ENOLCK, ENOTSUP, EOPNOTSUPP separately.
**
** 这一部分主要是将标志POSIX错误信息翻译为对sqlite3客户端
** 有效的东西，具体地说，就是将“重试”信息翻译为“请
** 立刻关闭文件描述符”存入SQLITE_IOERR。
** 在初始化锁期间或者是文件系统支持的锁应该分开处理
** ENOLCK, ENOTSUP, EOPNOTSUPP.
*/
static int sqliteErrorFromPosixError(int posixError, int sqliteIOErr) {
  switch (posixError) {
#if 0
  /* At one point this code was not commented out. In theory, this branch
  ** should never be hit, as this function should only be called after
  ** a locking-related function (i.e. fcntl()) has returned non-zero with
  ** the value of errno as the first argument. Since a system call has failed,
  ** errno should be non-zero.
  **
  ** Despite this, if errno really is zero, we still don't want to return
  ** SQLITE_OK. The system call failed, and *some* SQLite error should be
  ** propagated back to the caller. Commenting this branch out means errno==0
  ** will be handled by the "default:" case below.
  */
  case 0: 
    return SQLITE_OK;
#endif

  case EAGAIN:
  case ETIMEDOUT:
  case EBUSY:
  case EINTR:
  case ENOLCK:  
    /* random NFS retry error, unless during file system support 
     * introspection, in which it actually means what it says 
     **
     ** NFS随机重试错误，除非文件系统支持自省，就像
     ** 它的字面意思
     */
    return SQLITE_BUSY;
    
  case EACCES: 
    /* EACCES is like EAGAIN during locking operations, but not any other time
	** EACCES在锁操作过程中就像EAGAIN，其他时候就不一样*/
    if( (sqliteIOErr == SQLITE_IOERR_LOCK) || 
        (sqliteIOErr == SQLITE_IOERR_UNLOCK) || 
        (sqliteIOErr == SQLITE_IOERR_RDLOCK) ||
        (sqliteIOErr == SQLITE_IOERR_CHECKRESERVEDLOCK) ){
      return SQLITE_BUSY;
    }
    /* else fall through */
  case EPERM: 
    return SQLITE_PERM;
    
  /* EDEADLK is only possible if a call to fcntl(F_SETLKW) is made. And
  ** this module never makes such a call. And the code in SQLite itself 
  ** asserts that SQLITE_IOERR_BLOCKED is never returned. For these reasons
  ** this case is also commented out. If the system does set errno to EDEADLK,
  ** the default SQLITE_IOERR_XXX code will be returned. 
  **
  ** EDEADLK是唯一可以调用fcntl(F_SETLKW)，而且从来也不会
  ** 主动发出这样的调用，就SQLite自身的断言SQLITE_IOERR_
  ** BLOCKED也不会返回这样的值。由于这些原因我们也需要
  ** 注明这个情况，如果系统没有设置errno到EDEADLK，将会
  ** 返回默认的SQLITE_IOERR_XXX代码
  */
#if 0
  case EDEADLK:
    return SQLITE_IOERR_BLOCKED;
#endif
    
#if EOPNOTSUPP!=ENOTSUP
  case EOPNOTSUPP: 
    /* something went terribly awry, unless during file system support 
     * introspection, in which it actually means what it says 
     ** 有时候出现可怕的错误，除非文件系统支持
     ** 自省，就像字面上表示的那样
     */
#endif
#ifdef ENOTSUP
  case ENOTSUP: 
    /* invalid fd, unless during file system support introspection, in which 
     * it actually means what it says 
     **
     ** fd无效，除非文件系统支持自省，就像字面表示*/
#endif
  case EIO:
  case EBADF:
  case EINVAL:
  case ENOTCONN:
  case ENODEV:
  case ENXIO:
  case ENOENT:
#ifdef ESTALE                     /* ESTALE is not defined on Interix systems 
								  ** 在Interix系统中没有定义ESTALE  
								  */
  case ESTALE:
#endif
  case ENOSYS:
    /* these should force the client to close the file and reconnect 
	** 这些强制客户端关闭文件进行重新连接
	*/
    
  default: 
    return sqliteIOErr;
  }
}



/******************************************************************************
****************** Begin Unique File ID Utility Used By VxWorks ***************
**
** 				    VxWorks中使用的独特文件ID
**
** On most versions of unix, we can get a unique ID for a file by concatenating
** the device number and the inode number.  But this does not work on VxWorks.
** On VxWorks, a unique file id must be based on the canonical filename.
**
** A pointer to an instance of the following structure can be used as a
** unique file ID in VxWorks.  Each instance of this structure contains
** a copy of the canonical filename.  There is also a reference count.  
** The structure is reclaimed when the number of pointers to it drops to
** zero.
**
** There are never very many files open at one time and lookups are not
** a performance-critical path, so it is sufficient to put these
** structures on a linked list.
** 
** 在大多数的unix版本下，我们可以通过连接到一个文件
** 的唯一ID设备编号和索引节点号，但是在VxWorks上这种
** 策略不起作用，必须使用规范的文件名获得标准ID。
** 指向以下数据结构的指针可以作为独特的文件ID存在VxWorks
** 中，每一个实例都包含一个规范文件名的副本。也包括一个
** 引用计数器。这个结构 被回收的时候，指针的数量也会下降
** 不会出现很多文件在同时被打开而查找不到一个关键路径
** 所以将这些数据结构链接起来是高效的。
*/
struct vxworksFileId {
  struct vxworksFileId *pNext;  /* Next in a list of them all */
  int nRef;                     /* Number of references to this one */
  int nName;                    /* Length of the zCanonicalName[] string */
  char *zCanonicalName;         /* Canonical filename */
};

#if OS_VXWORKS
/* 
** All unique filenames are held on a linked list headed by this
** variable:
** 所有不同的文件名保存在链表中以此结构开始。
*/
static struct vxworksFileId *vxworksFileList = 0;

/*
** Simplify a filename into its canonical form
** by making the following changes:
**
**  * removing any trailing and duplicate /
**  * convert /./ into just /
**  * convert /A/../ where A is any simple name into just /
**
** Changes are made in-place.  Return the new name length.
**
** The original filename is in z[0..n-1].  Return the number of
** characters in the simplified name.
**
** 将文件名根据这些规则简化为规范格式：
** * 移除任何附加和复制/
** * 转换/./为/
** * 转换/A/../A仅仅是/后的简单名称
** 在恰当的地方做出改变，返回新名自的长度
** 原始的文件名在z[0..n-1].返回简化后的字符数
*/
static int vxworksSimplifyName(char *z, int n){
  int i, j;
  while( n>1 && z[n-1]=='/' ){ n--; }
  for(i=j=0; i<n; i++){
    if( z[i]=='/' ){
      if( z[i+1]=='/' ) continue;
      if( z[i+1]=='.' && i+2<n && z[i+2]=='/' ){
        i += 1;
        continue;
      }
      if( z[i+1]=='.' && i+3<n && z[i+2]=='.' && z[i+3]=='/' ){
        while( j>0 && z[j-1]!='/' ){ j--; }
        if( j>0 ){ j--; }
        i += 2;
        continue;
      }
    }
    z[j++] = z[i];
  }
  z[j] = 0;
  return j;
}

/*
** Find a unique file ID for the given absolute pathname.  Return
** a pointer to the vxworksFileId object.  This pointer is the unique
** file ID.
**
** The nRef field of the vxworksFileId object is incremented before
** the object is returned.  A new vxworksFileId object is created
** and added to the global list if necessary.
**
** If a memory allocation error occurs, return NULL.
**
** 找到给定的绝对路径的文件ID，返回指向vxworksFileID
** 对象的指针。这个指针拥有一个独一无二的文件ID.
** 这个vxworksFileId对象的nRef范围需要加上此前返回对象的
** 长度。必要的时候还需要创建一个新的vxworkFileId对象
** 并添加到全局列表中。
** 如果内存分配时出现了错误，返回值为空
*/
static struct vxworksFileId *vxworksFindFileId(const char *zAbsoluteName){
  struct vxworksFileId *pNew;         /* search key and new file ID */
  struct vxworksFileId *pCandidate;   /* For looping over existing file IDs */
  int n;                              /* Length of zAbsoluteName string */

  assert( zAbsoluteName[0]=='/' );
  n = (int)strlen(zAbsoluteName);
  pNew = sqlite3_malloc( sizeof(*pNew) + (n+1) );
  if( pNew==0 ) return 0;
  pNew->zCanonicalName = (char*)&pNew[1];
  memcpy(pNew->zCanonicalName, zAbsoluteName, n+1);
  n = vxworksSimplifyName(pNew->zCanonicalName, n);

  /* Search for an existing entry that matching the canonical name.
  ** If found, increment the reference count and return a pointer to
  ** the existing file ID.
  **
  ** 搜索和标准名称匹配的现有条目，如果找到，
  ** 增加引用计数并返回一个指向现存文件ID的指针。
  */
  unixEnterMutex();
  for(pCandidate=vxworksFileList; pCandidate; pCandidate=pCandidate->pNext){
    if( pCandidate->nName==n 
     && memcmp(pCandidate->zCanonicalName, pNew->zCanonicalName, n)==0
    ){
       sqlite3_free(pNew);
       pCandidate->nRef++;
       unixLeaveMutex();
       return pCandidate;
    }
  }

  /* No match was found.  We will make a new file ID 
  ** 如果没有匹配的项目，创建一个新的文件ID
  */
  pNew->nRef = 1;
  pNew->nName = n;
  pNew->pNext = vxworksFileList;
  vxworksFileList = pNew;
  unixLeaveMutex();
  return pNew;
}

/*
** Decrement the reference count on a vxworksFileId object.  Free
** the object when the reference count reaches zero.
** 
** 减少在vxworksFileId对象上面的引用计数，在计数值
** 为0时释放这个对象
*/
static void vxworksReleaseFileId(struct vxworksFileId *pId){
  unixEnterMutex();
  assert( pId->nRef>0 );
  pId->nRef--;
  if( pId->nRef==0 ){
    struct vxworksFileId **pp;
    for(pp=&vxworksFileList; *pp && *pp!=pId; pp = &((*pp)->pNext)){}
    assert( *pp==pId );
    *pp = pId->pNext;
    sqlite3_free(pId);
  }
  unixLeaveMutex();
}
#endif /* OS_VXWORKS */
/*************** End of Unique File ID Utility Used By VxWorks ****************
******************************************************************************/


/******************************************************************************
*************************** Posix Advisory Locking ****************************
**
**                                          Posix的锁建议
**
** POSIX advisory locks are broken by design.  ANSI STD 1003.1 (1996)
** section 6.5.2.2 lines 483 through 490 specify that when a process
** sets or clears a lock, that operation overrides any prior locks set
** by the same process.  It does not explicitly say so, but this implies
** that it overrides locks set by the same process using a different
** file descriptor.  Consider this test case:
**
**       int fd1 = open("./file1", O_RDWR|O_CREAT, 0644);
**       int fd2 = open("./file2", O_RDWR|O_CREAT, 0644);
**
** Suppose ./file1 and ./file2 are really the same file (because
** one is a hard or symbolic link to the other) then if you set
** an exclusive lock on fd1, then try to get an exclusive lock
** on fd2, it works.  I would have expected the second lock to
** fail since there was already a lock on the file due to fd1.
** But not so.  Since both locks came from the same process, the
** second overrides the first, even though they were on different
** file descriptors opened on different file names.
**
** POSIX的锁建议通过设计而分开，ANSI STD 1003.1(1996)文件
** 6.5.2.2节483行到490行规定一个进程设置或者清除一个
** 锁的时候，操作优先于同进程的其他任何操作。
** 虽然没有明确写明，但是表明通过不同的文件描述符
** 实现了对文件锁的重载。考虑这个测试用例：
**		int fd1 = open("./file1", O_RDWR|O_CREAT, 0644);
**          int fd2 = open("./file2". O_RDWR|O_CREAT, 0644);
** 假设./file1和./file2实际上就是同一文件(因为其中一个是
** 硬连接或者象征性链接到其他对象)然后，如果你对
** fd1设置独占锁，然后尝试也确实获取了fd2的独占锁，
** 我本希望第二个操作失败，因为第一个锁已经获得、
** 但实际上不是这样，由于两个锁来自同一个进程，
** 所以第二个会覆盖第一个锁。虽然他们是不同的文件
** 描述符打开的不同文件。
**
** This means that we cannot use POSIX locks to synchronize file access
** among competing threads of the same process.  POSIX locks will work fine
** to synchronize access for threads in separate processes, but not
** threads within the same process.
**
** To work around the problem, SQLite has to manage file locks internally
** on its own.  Whenever a new database is opened, we have to find the
** specific inode of the database file (the inode is determined by the
** st_dev and st_ino fields of the stat structure that fstat() fills in)
** and check for locks already existing on that inode.  When locks are
** created or removed, we have to look at our own internal record of the
** locks to see if another thread has previously set a lock on that same
** inode.
**
** (Aside: The use of inode numbers as unique IDs does not work on VxWorks.
** For VxWorks, we have to use the alternative unique ID system based on
** canonical filename and implemented in the previous division.)
**
** 这意味着我们不能使用POSIX锁在同一进程的竞争线程间
** 实现同步文件。POSIX锁在不同进程间实现文件同步是比
** 较好的方法。但是不能在同一进程的不同线程中使用。
**
** 为了解决这个问题，SQLite在内部实现了自己的文件锁，
** 无论何时一个新的数据库被打开，我们需要找到对应的
** 文件索引节点(是由fstat()在文件初始st_dev和st_ino域包括的inode
** 统计结果）同时检查已存在的索引节点锁。当锁被创建
** 或者移除的时候，我们需要查看锁的内部记录，是否有
** 其他的线程已经设置了同样的inode，
**（旁白：iNode编号作为文件的标识在VxWorks上不起作用，我们
** 需要基于典型的文件名和此前的划分实现可取的ID系统。
**
** The sqlite3_file structure for POSIX is no longer just an integer file
** descriptor.  It is now a structure that holds the integer file
** descriptor and a pointer to a structure that describes the internal
** locks on the corresponding inode.  There is one locking structure
** per inode, so if the same inode is opened twice, both unixFile structures
** point to the same locking structure.  The locking structure keeps
** a reference count (so we will know when to delete it) and a "cnt"
** field that tells us its internal lock status.  cnt==0 means the
** file is unlocked.  cnt==-1 means the file has an exclusive lock.
** cnt>0 means there are cnt shared locks on the file.
**
** Any attempt to lock or unlock a file first checks the locking
** structure.  The fcntl() system call is only invoked to set a 
** POSIX lock if the internal lock structure transitions between
** a locked and an unlocked state.
**
** But wait:  there are yet more problems with POSIX advisory locks.
**
** POSIX中sqlite3_file结构不再仅仅是一个整数文件描述符，现在
** 是一个包含有文件描述符和指向内部iNode结构的指针。对
** 用于每一个iNode结构有一个锁定结构，如果同一个iNode 被
** 两次打开，两个unixFile对象都会指向同一个锁结构。所以
** 必须要保持一个引用计数对象（这样我们就知道什么时候
** 对它进行释放）和一个"cnt"字段表示锁定的状态。为0表示
** 文件没有被锁住，大于0表示文件上有cnt锁。
** 任何尝试锁住或者对文件解锁的操作首先都要检查文件的
** 锁结构，系统调用fcntl()只是在POSIX锁内部进行状态的转换。
**
** If you close a file descriptor that points to a file that has locks,
** all locks on that file that are owned by the current process are
** released.  To work around this problem, each unixInodeInfo object
** maintains a count of the number of pending locks on tha inode.
** When an attempt is made to close an unixFile, if there are
** other unixFile open on the same inode that are holding locks, the call
** to close() the file descriptor is deferred until all of the locks clear.
** The unixInodeInfo structure keeps a list of file descriptors that need to
** be closed and that list is walked (and cleared) when the last lock
** clears.
**
** Yet another problem:  LinuxThreads do not play well with posix locks.
**
** Many older versions of linux use the LinuxThreads library which is
** not posix compliant.  Under LinuxThreads, a lock created by thread
** A cannot be modified or overridden by a different thread B.
** Only thread A can modify the lock.  Locking behavior is correct
** if the appliation uses the newer Native Posix Thread Library (NPTL)
** on linux - with NPTL a lock created by thread A can override locks
** in thread B.  But there is no way to know at compile-time which
** threading library is being used.  So there is no way to know at
** compile-time whether or not thread A can override locks on thread B.
** One has to do a run-time check to discover the behavior of the
** current process.
**
** 如果你关闭了一个有锁的指向文件描述符，当前进程拥有的
** 所有锁都会被释放。为了解决这个问题，不同的unixInoeInfo对象
** 保持在iNode上的pending锁计数。在尝试关闭unixFile时，如果其他的
** unixFile在打开同一个文件且含有锁时，这个文件描述符关闭操作
** 就会被推迟到所有的文件锁都被清除.unixInodeInfo结构保持文件描
** 述符列表，在最后一个锁被销毁时，列表关闭。
** 然而还有一个问题：Linux线程在POSIX锁下运行效果不好。
** 许多Linux旧版本使用Linux线程库并不支持POSIX，在Linux线程中，一
** 个线程创建的锁不能被其他的线程修改或者改写。只有创建的
** 线程才能修改这个锁，如果应用遵循NPTL原则则锁定是安全的
** 由线程A创建的锁可以覆盖线程B创建的锁。但是在编译时无法获知
** 当前使用的线程库，所以无法知道编译后线程A 可以重载线程B的锁
** 我们必须进行运行时检查当前进程行为的正确性
**
** SQLite used to support LinuxThreads.  But support for LinuxThreads
** was dropped beginning with version 3.7.0.  SQLite will still work with
** LinuxThreads provided that (1) there is no more than one connection 
** per database file in the same process and (2) database connections
** do not move across threads.
** 
** SQLite曾经支持Linux线程库但是从3.7.0版本开始不再支持。SQLite将会继续
** 使用Linux线程库的服务（1）同一个进程中不会超过一个连接（2）数
** 据库连接不会穿越线程
*/

/*
** An instance of the following structure serves as the key used
** to locate a particular unixInodeInfo object.
**
** 下列实例作为特定unixInodeInfo对象的密钥
*/
struct unixFileId {
  dev_t dev;                  /* Device number */
#if OS_VXWORKS
  struct vxworksFileId *pId;  /* Unique file ID for vxworks. */
#else
  ino_t ino;                  /* Inode number */
#endif
};

/*
** An instance of the following structure is allocated for each open
** inode.  Or, on LinuxThreads, there is one of these structures for
** each inode opened by each thread.
**
** A single inode can have multiple file descriptors, so each unixFile
** structure contains a pointer to an instance of this object and this
** object keeps a count of the number of unixFile pointing to it.
**
** 为每一个iNode分配实例或者在Linux线程中，这些结构
** 都是为iNode和线程对应。
** 单个iNode可以拥有多个文件描述符，所以每个unixFile
** 结构包括指向这个实例的指针，同时保持着unixFile
** 指向他的计数器。
*/
struct unixInodeInfo {
  struct unixFileId fileId;       /* The lookup key */
  int nShared;                    /* Number of SHARED locks held */
  unsigned char eFileLock;        /* One of SHARED_LOCK, RESERVED_LOCK etc. */
  unsigned char bProcessLock;     /* An exclusive process lock is held */
  int nRef;                       /* Number of pointers to this structure */
  unixShmNode *pShmNode;          /* Shared memory associated with this inode */
  int nLock;                      /* Number of outstanding file locks */
  UnixUnusedFd *pUnused;          /* Unused file descriptors to close */
  unixInodeInfo *pNext;           /* List of all unixInodeInfo objects */
  unixInodeInfo *pPrev;           /*    .... doubly linked */
#if SQLITE_ENABLE_LOCKING_STYLE
  unsigned long long sharedByte;  /* for AFP simulated shared lock */
#endif
#if OS_VXWORKS
  sem_t *pSem;                    /* Named POSIX semaphore */
  char aSemName[MAX_PATHNAME+2];  /* Name of that semaphore */
#endif
};

/*
** A lists of all unixInodeInfo objects. 所有unixInodeInfo对象列表
*/
static unixInodeInfo *inodeList = 0;

/*
**
** This function - unixLogError_x(), is only ever called via the macro
** unixLogError().
**
** It is invoked after an error occurs in an OS function and errno has been
** set. It logs a message using sqlite3_log() containing the current value of
** errno and, if possible, the human-readable equivalent from strerror() or
** strerror_r().
**
** The first argument passed to the macro should be the error code that
** will be returned to SQLite (e.g. SQLITE_IOERR_DELETE, SQLITE_CANTOPEN). 
** The two subsequent arguments should be the name of the OS function that
** failed (e.g. "unlink", "open") and the associated file-system path,
** if any.
**
** unixLogError_x()方法仅仅是通过调用unixLogError()宏，
** 这些调用主要是在操作系统层出现错误后进行，
** 使用sqlite3_log()记录当前错误值，在需要的时候，从
** strerror()或strerror_r()获得人类可以阅读的信息。
** 传递给宏的第一个参数应该是SQLite返回的错误代码，
** （SQLITE_IOERR_DELETE, SQLITE_CANTOPEN）接下来的两个参数
** 应该是操作系统的函数失败时的名称和相关路径。
*/
#define unixLogError(a,b,c)     unixLogErrorAtLine(a,b,c,__LINE__)
static int unixLogErrorAtLine(
  int errcode,                    /* SQLite error code */
  const char *zFunc,              /* Name of OS function that failed */
  const char *zPath,              /* File path associated with error */
  int iLine                       /* Source line number where error occurred */
){
  char *zErr;                     /* Message from strerror() or equivalent */
  int iErrno = errno;             /* Saved syscall error number */

  /* If this is not a threadsafe build (SQLITE_THREADSAFE==0), then use
  ** the strerror() function to obtain the human-readable error message
  ** equivalent to errno. Otherwise, use strerror_r().
  **
  ** 如果这个不是线程安全(SQLITE_THREADSAFE==0)，然后使用strerror()函数
  ** 得到可读的错误信息，或者使用strerror_r().
  */ 
#if SQLITE_THREADSAFE && defined(HAVE_STRERROR_R)
  char aErr[80];
  memset(aErr, 0, sizeof(aErr));
  zErr = aErr;

  /* If STRERROR_R_CHAR_P (set by autoconf scripts) or __USE_GNU is defined,
  ** assume that the system provides the GNU version of strerror_r() that
  ** returns a pointer to a buffer containing the error message. That pointer 
  ** may point to aErr[], or it may point to some static storage somewhere. 
  ** Otherwise, assume that the system provides the POSIX version of 
  ** strerror_r(), which always writes an error message into aErr[].
  **
  ** If the code incorrectly assumes that it is the POSIX version that is
  ** available, the error message will often be an empty string. Not a
  ** huge problem. Incorrectly concluding that the GNU version is available 
  ** could lead to a segfault though.
  ** 
  ** 如果STRERROR_R_CHAR_P(由脚本自动创建）或_USE_GNU定义，
  ** 假设系统提供GNU版本的strerror_r()返回一个指向存放错误
  ** 信息的缓冲区指针，该指针可能指向aErr[]。或者指向
  ** 一个静态存储的区域，否则，假设系统提供的POSIX strerror_()
  ** 版本，总是写一个错误信息到aErr[],
  ** 如果一个代码假设POSIX版本是可用的，错误信息就是空字符串，
  ** 不是一个大问题，错误的认为gnu版本可用，才是大问题
  ** 
  */
#if defined(STRERROR_R_CHAR_P) || defined(__USE_GNU)
  zErr = 
# endif
  strerror_r(iErrno, aErr, sizeof(aErr)-1);

#elif SQLITE_THREADSAFE
  /* This is a threadsafe build, but strerror_r() is not available. */
  zErr = "";
#else
  /* Non-threadsafe build, use strerror(). */
  zErr = strerror(iErrno);
#endif

  assert( errcode!=SQLITE_OK );
  if( zPath==0 ) zPath = "";
  sqlite3_log(errcode,
      "os_unix.c:%d: (%d) %s(%s) - %s",
      iLine, iErrno, zFunc, zPath, zErr
  );

  return errcode;
}

/*
** Close a file descriptor.
**
** We assume that close() almost always works, since it is only in a
** very sick application or on a very sick platform that it might fail.
** If it does fail, simply leak the file descriptor, but do log the
** error.
**
** Note that it is not safe to retry close() after EINTR since the
** file descriptor might have already been reused by another thread.
** So we don't even try to recover from an EINTR.  Just log the error
** and move on.
**
** 关闭文件描述符
** 我们假设close()一般情况下都可行，除非是运行在一个非常差
** 的应用或是平台才会导致错误。如果出错，导致文件泄露
** 也是会在log文件中记录。
** 注意，在EINTR之后再次尝试关闭文件是不安全的，这是因为
** 文件描述符可能已经被另一个线程占用。所有我们不需要
** 从EINTR中尝试再次关闭文件，只需要进行错误日志记录继续
** 就可以了
*/
static void robust_close(unixFile *pFile, int h, int lineno){
  if( osClose(h) ){
    unixLogErrorAtLine(SQLITE_IOERR_CLOSE, "close",
                       pFile ? pFile->zPath : 0, lineno);
  }
}

/*
** Close all file descriptors accumuated in the unixInodeInfo->pUnused list.
** 关闭unixInodeInfo->pUnused中所有的文件描述符列表
*/ 
static void closePendingFds(unixFile *pFile){
  unixInodeInfo *pInode = pFile->pInode;
  UnixUnusedFd *p;
  UnixUnusedFd *pNext;
  for(p=pInode->pUnused; p; p=pNext){
    pNext = p->pNext;
    robust_close(pFile, p->fd, __LINE__);
    sqlite3_free(p);
  }
  pInode->pUnused = 0;
}

/*
** Release a unixInodeInfo structure previously allocated by findInodeInfo().
**
** The mutex entered using the unixEnterMutex() function must be held
** when this function is called.
**
**  释放此前由findInodeInfo()分配的unixInodeInfo对象，
** 在unixEnterMutex()函数调用时必须保持mutex
*/
static void releaseInodeInfo(unixFile *pFile){
  unixInodeInfo *pInode = pFile->pInode;
  assert( unixMutexHeld() );
  if( ALWAYS(pInode) ){
    pInode->nRef--;
    if( pInode->nRef==0 ){
      assert( pInode->pShmNode==0 );
      closePendingFds(pFile);
      if( pInode->pPrev ){
        assert( pInode->pPrev->pNext==pInode );
        pInode->pPrev->pNext = pInode->pNext;
      }else{
        assert( inodeList==pInode );
        inodeList = pInode->pNext;
      }
      if( pInode->pNext ){
        assert( pInode->pNext->pPrev==pInode );
        pInode->pNext->pPrev = pInode->pPrev;
      }
      sqlite3_free(pInode);
    }
  }
}

/*
** Given a file descriptor, locate the unixInodeInfo object that
** describes that file descriptor.  Create a new one if necessary.  The
** return value might be uninitialized if an error occurs.
**
** The mutex entered using the unixEnterMutex() function must be held
** when this function is called.
**
** Return an appropriate error code.
**
** 对于给定的文件描述符，定位描述unixInodeInfo对象的具体位置，
** 必须的时候创建新对象。出现错误的时候返回值可能是随机值
** 互斥进入unixEnterMutex()函数时被调用。
** 返回值是一个恰当的错误代码
*/
static int findInodeInfo(
  unixFile *pFile,               /* Unix file with file desc used in the key */
  unixInodeInfo **ppInode        /* Return the unixInodeInfo object here */
){
  int rc;                        /* System call return code */
  int fd;                        /* The file descriptor for pFile */
  struct unixFileId fileId;      /* Lookup key for the unixInodeInfo */
  struct stat statbuf;           /* Low-level file information */
  unixInodeInfo *pInode = 0;     /* Candidate unixInodeInfo object */

  assert( unixMutexHeld() );

  /* Get low-level information about the file that we can used to
  ** create a unique name for the file.
  **
  ** 获得我们用来创建独特名称文件的低等级信息
  */
  fd = pFile->h;
  rc = osFstat(fd, &statbuf);
  if( rc!=0 ){
    pFile->lastErrno = errno;
#ifdef EOVERFLOW
    if( pFile->lastErrno==EOVERFLOW ) return SQLITE_NOLFS;
#endif
    return SQLITE_IOERR;
  }

#ifdef __APPLE__
  /* On OS X on an msdos filesystem, the inode number is reported
  ** incorrectly for zero-size files.  See ticket #3260.  To work
  ** around this problem (we consider it a bug in OS X, not SQLite)
  ** we always increase the file size to 1 by writing a single byte
  ** prior to accessing the inode number.  The one byte written is
  ** an ASCII 'S' character which also happens to be the first byte
  ** in the header of every SQLite database.  In this way, if there
  ** is a race condition such that another thread has already populated
  ** the first page of the database, no damage is done.
  **
  ** 在OS X的msdos文件系统上，对于比零大的iNode编号实际上
  ** 存在错误，详见#3260，为了解决这个问题（我们假设它是
  ** OS X上的一个bug，而不是来自SQLite)在访问对于iNode编号前，
  ** 我们总是通过写一个字节把文件的大小增加到1。字符"S"
  ** 也恰好是每个SQLite数据库头文件中的第一个字符。通过这种
  ** 方法，如果有一种类别条件，例如另外一个进程已经填充了
  ** 数据库的第一页，而没有造成损坏。
  */
  if( statbuf.st_size==0 && (pFile->fsFlags & SQLITE_FSFLAGS_IS_MSDOS)!=0 ){
    do{ rc = osWrite(fd, "S", 1); }while( rc<0 && errno==EINTR );
    if( rc!=1 ){
      pFile->lastErrno = errno;
      return SQLITE_IOERR;
    }
    rc = osFstat(fd, &statbuf);
    if( rc!=0 ){
      pFile->lastErrno = errno;
      return SQLITE_IOERR;
    }
  }
#endif

  memset(&fileId, 0, sizeof(fileId));
  fileId.dev = statbuf.st_dev;
#if OS_VXWORKS
  fileId.pId = pFile->pId;
#else
  fileId.ino = statbuf.st_ino;
#endif
  pInode = inodeList;
  while( pInode && memcmp(&fileId, &pInode->fileId, sizeof(fileId)) ){
    pInode = pInode->pNext;
  }
  if( pInode==0 ){
    pInode = sqlite3_malloc( sizeof(*pInode) );
    if( pInode==0 ){
      return SQLITE_NOMEM;
    }
    memset(pInode, 0, sizeof(*pInode));
    memcpy(&pInode->fileId, &fileId, sizeof(fileId));
    pInode->nRef = 1;
    pInode->pNext = inodeList;
    pInode->pPrev = 0;
    if( inodeList ) inodeList->pPrev = pInode;
    inodeList = pInode;
  }else{
    pInode->nRef++;
  }
  *ppInode = pInode;
  return SQLITE_OK;
}


/*
** This routine checks if there is a RESERVED lock held on the specified
** file by this or any other process. If such a lock is held, set *pResOut
** to a non-zero value otherwise *pResOut is set to zero.  The return value
** is set to SQLITE_OK unless an I/O error occurs during lock checking.
**
** 主要是用于检查特定文件中被本进程或其他进程获得的预留锁
** 如果持有这样的锁，设置*pResOut为非0值否则设置为0.返回值为
** SQLITE_OK除非在锁检查时发生了错误。
*/
static int unixCheckReservedLock(sqlite3_file *id, int *pResOut){
  int rc = SQLITE_OK;
  int reserved = 0;
  unixFile *pFile = (unixFile*)id;

  SimulateIOError( return SQLITE_IOERR_CHECKRESERVEDLOCK; );

  assert( pFile );
  unixEnterMutex(); /* Because pFile->pInode is shared across threads */

  /* Check if a thread in this process holds such a lock */
  if( pFile->pInode->eFileLock>SHARED_LOCK ){
    reserved = 1;
  }

  /* Otherwise see if some other process holds it.
  */
#ifndef __DJGPP__
  if( !reserved && !pFile->pInode->bProcessLock ){
    struct flock lock;
    lock.l_whence = SEEK_SET;
    lock.l_start = RESERVED_BYTE;
    lock.l_len = 1;
    lock.l_type = F_WRLCK;
    if( osFcntl(pFile->h, F_GETLK, &lock) ){
      rc = SQLITE_IOERR_CHECKRESERVEDLOCK;
      pFile->lastErrno = errno;
    } else if( lock.l_type!=F_UNLCK ){
      reserved = 1;
    }
  }
#endif
  
  unixLeaveMutex();
  OSTRACE(("TEST WR-LOCK %d %d %d (unix)\n", pFile->h, rc, reserved));

  *pResOut = reserved;
  return rc;
}

/*
** Attempt to set a system-lock on the file pFile.  The lock is 
** described by pLock.
**
** If the pFile was opened read/write from unix-excl, then the only lock
** ever obtained is an exclusive lock, and it is obtained exactly once
** the first time any lock is attempted.  All subsequent system locking
** operations become no-ops.  Locking operations still happen internally,
** in order to coordinate access between separate database connections
** within this process, but all of that is handled in memory and the
** operating system does not participate.
**
** This function is a pass-through to fcntl(F_SETLK) if pFile is using
** any VFS other than "unix-excl" or if pFile is opened on "unix-excl"
** and is read-only.
**
** Zero is returned if the call completes successfully, or -1 if a call
** to fcntl() fails. In this case, errno is set appropriately (by fcntl()).
**
** 在pFile文件上尝试设置一个系统文件锁，使用pLock描述
** 如果pFile由unix-excl读/写,然后获得唯一锁就是独占锁，也
** 只会在每个锁第一次尝试的时候获得，所有后续的系统
** 锁操作都只会变得无意义，锁操作仍然发生在文件内部
** 为了在本进程中获得分离的数据库访问接口，但是这些
** 都需要在内存中进行，操作系统不参与其中。
** 如果pFile使用除了"unix-excl"之外的VFS是通过fcntl实现的。如果
** pFile是基于"unix-excl"的话，他就是只读的。
** 如果调用成功返回值为0，调用fcntl()失败返回1，在这种情况下，
** errno就要被正确的设置（由fcntl()设置）
*/
static int unixFileLock(unixFile *pFile, struct flock *pLock){
  int rc;
  unixInodeInfo *pInode = pFile->pInode;
  assert( unixMutexHeld() );
  assert( pInode!=0 );
  if( ((pFile->ctrlFlags & UNIXFILE_EXCL)!=0 || pInode->bProcessLock)
   && ((pFile->ctrlFlags & UNIXFILE_RDONLY)==0)
  ){
    if( pInode->bProcessLock==0 ){
      struct flock lock;
      assert( pInode->nLock==0 );
      lock.l_whence = SEEK_SET;
      lock.l_start = SHARED_FIRST;
      lock.l_len = SHARED_SIZE;
      lock.l_type = F_WRLCK;
      rc = osFcntl(pFile->h, F_SETLK, &lock);
      if( rc<0 ) return rc;
      pInode->bProcessLock = 1;
      pInode->nLock++;
    }else{
      rc = 0;
    }
  }else{
    rc = osFcntl(pFile->h, F_SETLK, pLock);
  }
  return rc;
}

/*
** Lock the file with the lock specified by parameter eFileLock - one
** of the following:
**
**     (1) SHARED_LOCK
**     (2) RESERVED_LOCK
**     (3) PENDING_LOCK
**     (4) EXCLUSIVE_LOCK
** 根据以下的锁实现并行处理，主要有以下的：
**     （1）共享锁
**     （2）预订锁
**     （3）意向锁
**     （4）独占锁
**
** Sometimes when requesting one lock state, additional lock states
** are inserted in between.  The locking might fail on one of the later
** transitions leaving the lock state different from what it started but
** still short of its goal.  The following chart shows the allowed
** transitions and the inserted intermediate states:
**
** 有时在请求一个锁定是，在锁定状态确定的中间阶段
** 由于之后一个锁的状态不同导致失败，没有到达预期
** 结果，下面的表格显示了允许的转换和插入状态：
**
**    UNLOCKED -> SHARED
**    SHARED -> RESERVED
**    SHARED -> (PENDING) -> EXCLUSIVE
**    RESERVED -> (PENDING) -> EXCLUSIVE
**    PENDING -> EXCLUSIVE
**
** 解锁-> 共享
** 共享-> 保留
** 共享->（待定)->独占
** 保留->(待定)->独占
** 挂起-> 独占
**
** This routine will only increase a lock.  Use the sqlite3OsUnlock()
** routine to lower a locking level.
** 
** 这个路线只会增加一个锁，winUnlock()函数会立即清除
** 所有的锁给我们返回0级，我们不可能逐步降低锁
** 你必须一次性锁定为0级
*/

static int unixLock(sqlite3_file *id, int eFileLock){
  /* The following describes the implementation of the various locks and
  ** lock transitions in terms of the POSIX advisory shared and exclusive
  ** lock primitives (called read-locks and write-locks below, to avoid
  ** confusion with SQLite lock names). The algorithms are complicated
  ** slightly in order to be compatible with windows systems simultaneously
  ** accessing the same database file, in case that is ever required.
  **
  ** Symbols defined in os.h indentify the 'pending byte' and the 'reserved
  ** byte', each single bytes at well known offsets, and the 'shared byte
  ** range', a range of 510 bytes at a well known offset.
  **
  ** To obtain a SHARED lock, a read-lock is obtained on the 'pending
  ** byte'.  If this is successful, a random byte from the 'shared byte
  ** range' is read-locked and the lock on the 'pending byte' released.
  **
  ** 下面主要介绍各种锁的实现和POSIX中各种锁的转换，
  ** 共享锁和独占锁原子（接下来称之为读锁和写锁，
  ** 从而避免和SQLite锁名称混淆），这个算法有一些复杂，
  ** 主要是为了和Windows系统兼容，需要时访问同一个数据库。
  ** 符号主要是在os.h文件中定义”pending字节“和”保留的字节“
  ** 每一个字节都有偏移量，和共享内存范围，“主要有
  ** 510字节的偏移范围。
  ** 为了获得共享锁，需要在挂起字节中获得读锁，如果有效，
  ** 就会在共享字节区域获得一个字节，进行读锁定和释放。
  **
  ** A process may only obtain a RESERVED lock after it has a SHARED lock.
  ** A RESERVED lock is implemented by grabbing a write-lock on the
  ** 'reserved byte'. 
  **
  ** A process may only obtain a PENDING lock after it has obtained a
  ** SHARED lock. A PENDING lock is implemented by obtaining a write-lock
  ** on the 'pending byte'. This ensures that no new SHARED locks can be
  ** obtained, but existing SHARED locks are allowed to persist. A process
  ** does not have to obtain a RESERVED lock on the way to a PENDING lock.
  ** This property is used by the algorithm for rolling back a journal file
  ** after a crash.
  **
  ** An EXCLUSIVE lock, obtained after a PENDING lock is held, is
  ** implemented by obtaining a write-lock on the entire 'shared byte
  ** range'. Since all other locks require a read-lock on one of the bytes
  ** within this range, this ensures that no other locks are held on the
  ** database. 
  **
  ** The reason a single byte cannot be used instead of the 'shared byte
  ** range' is that some versions of windows do not support read-locks. By
  ** locking a random byte from a range, concurrent SHARED locks may exist
  ** even if the locking primitive used is always a write-lock.
  **
  ** 进程只能在获得共享锁之后获得保留锁，一个保留锁
  ** 是通过在获取写锁的保留字节获得。
  **
  ** 一个进程只有在获得一个挂起锁之后才能获得共享锁。
  ** 一个共享锁只有在’共享字节‘中获得。但是已经获得
  ** 共享锁，通过获取挂起锁确保不会再次获得共享锁，已经
  ** 存在的共享锁可以保留，一个进程可以在没有获得挂起锁
  ** 的情况下获得保留锁，这个主要用在程序出错日志文件回滚
  **
  ** 一个互斥锁需要在获得挂起锁之后获得，是通过获得整个
  ** 共享字节的写锁实现。由于其他锁都需要对其中的几个字节
  **进行读锁定，从而确保没有在这个区域内数据库没有其他的锁
  **
  ** 不能使用单字节而是用共享字节，这是因为某些版本的Windows
  ** 不支持读锁，当前的共享锁在很大程度上是写锁，
  */
  int rc = SQLITE_OK;
  unixFile *pFile = (unixFile*)id;
  unixInodeInfo *pInode;
  struct flock lock;
  int tErrno = 0;

  assert( pFile );
  OSTRACE(("LOCK    %d %s was %s(%s,%d) pid=%d (unix)\n", pFile->h,
      azFileLock(eFileLock), azFileLock(pFile->eFileLock),
      azFileLock(pFile->pInode->eFileLock), pFile->pInode->nShared , getpid()));

  /* If there is already a lock of this type or more restrictive on the
  ** unixFile, do nothing. Don't use the end_lock: exit path, as
  ** unixEnterMutex() hasn't been called yet.
  ** 如果在unixFile上已经存在这种类型的或是更加严格的锁
  ** 那么什么也不做，就像unixEnterMutex()什么也没做一样退出
  ** 当前路径。
  */
  if( pFile->eFileLock>=eFileLock ){
    OSTRACE(("LOCK    %d %s ok (already held) (unix)\n", pFile->h,
            azFileLock(eFileLock)));
    return SQLITE_OK;
  }

  /* Make sure the locking sequence is correct.
  **  (1) We never move from unlocked to anything higher than shared lock.
  **  (2) SQLite never explicitly requests a pendig lock.
  **  (3) A shared lock is always held when a reserve lock is requested.
  **
  ** 确保锁序列的正确性。
  ** (1) 我们永远不会解锁高于共享锁
  ** (2) SQLite从来不会显示请求pending锁
  ** (3) 在请求保留锁时，永远保持共享锁
  */
  assert( pFile->eFileLock!=NO_LOCK || eFileLock==SHARED_LOCK );
  assert( eFileLock!=PENDING_LOCK );
  assert( eFileLock!=RESERVED_LOCK || pFile->eFileLock==SHARED_LOCK );

  /* This mutex is needed because pFile->pInode is shared across threads
  */
  unixEnterMutex();
  pInode = pFile->pInode;

  /* If some thread using this PID has a lock via a different unixFile*
  ** handle that precludes the requested lock, return BUSY.
  **
  ** 如果一些线程使用由不同unixFile*获得锁的PID句柄
  ** 从而获得排它锁，返回BUSY
  */
  if( (pFile->eFileLock!=pInode->eFileLock && 
          (pInode->eFileLock>=PENDING_LOCK || eFileLock>SHARED_LOCK))
  ){
    rc = SQLITE_BUSY;
    goto end_lock;
  }

  /* If a SHARED lock is requested, and some thread using this PID already
  ** has a SHARED or RESERVED lock, then increment reference counts and
  ** return SQLITE_OK.
  **
  ** 如果请求一个共享锁，且部分线程已经在使用这个PID，
  ** 而且其中已经包含共享或者预留锁，然后需要增加其
  ** 引用计数，返回SQLITE_OK。
  */
  if( eFileLock==SHARED_LOCK && 
      (pInode->eFileLock==SHARED_LOCK || pInode->eFileLock==RESERVED_LOCK) ){
    assert( eFileLock==SHARED_LOCK );
    assert( pFile->eFileLock==0 );
    assert( pInode->nShared>0 );
    pFile->eFileLock = SHARED_LOCK;
    pInode->nShared++;
    pInode->nLock++;
    goto end_lock;
  }


  /* A PENDING lock is needed before acquiring a SHARED lock and before
  ** acquiring an EXCLUSIVE lock.  For the SHARED lock, the PENDING will
  ** be released.
  **
  ** 在获取共享锁前需要获得pending锁，获取独占锁之前也是
  ** 这样的，对于共享锁，pending锁就会被释放。
  */
  lock.l_len = 1L;
  lock.l_whence = SEEK_SET;
  if( eFileLock==SHARED_LOCK 
      || (eFileLock==EXCLUSIVE_LOCK && pFile->eFileLock<PENDING_LOCK)
  ){
    lock.l_type = (eFileLock==SHARED_LOCK?F_RDLCK:F_WRLCK);
    lock.l_start = PENDING_BYTE;
    if( unixFileLock(pFile, &lock) ){
      tErrno = errno;
      rc = sqliteErrorFromPosixError(tErrno, SQLITE_IOERR_LOCK);
      if( rc!=SQLITE_BUSY ){
        pFile->lastErrno = tErrno;
      }
      goto end_lock;
    }
  }


  /* If control gets to this point, then actually go ahead and make
  ** operating system calls for the specified lock.
  ** 如果控制结构到达这个点，实际上就是要求操作系统
  ** 调用指定的锁
  */
  if( eFileLock==SHARED_LOCK ){
    assert( pInode->nShared==0 );
    assert( pInode->eFileLock==0 );
    assert( rc==SQLITE_OK );

    /* Now get the read-lock  这样就获得了读锁*/
    lock.l_start = SHARED_FIRST;
    lock.l_len = SHARED_SIZE;
    if( unixFileLock(pFile, &lock) ){
      tErrno = errno;
      rc = sqliteErrorFromPosixError(tErrno, SQLITE_IOERR_LOCK);
    }

    /* Drop the temporary PENDING lock  释放临时的pending锁*/
    lock.l_start = PENDING_BYTE;
    lock.l_len = 1L;
    lock.l_type = F_UNLCK;
    if( unixFileLock(pFile, &lock) && rc==SQLITE_OK ){
      /* This could happen with a network mount  在连接网络时发生*/
      tErrno = errno;
      rc = SQLITE_IOERR_UNLOCK; 
    }

    if( rc ){
      if( rc!=SQLITE_BUSY ){
        pFile->lastErrno = tErrno;
      }
      goto end_lock;
    }else{
      pFile->eFileLock = SHARED_LOCK;
      pInode->nLock++;
      pInode->nShared = 1;
    }
  }else if( eFileLock==EXCLUSIVE_LOCK && pInode->nShared>1 ){
    /* We are trying for an exclusive lock but another thread in this
    ** same process is still holding a shared lock. 
    ** 我们申请一个独占锁但是同一进城的不同线程任然占有这个共享锁
    */
    rc = SQLITE_BUSY;
  }else{
    /* The request was for a RESERVED or EXCLUSIVE lock.  It is
    ** assumed that there is a SHARED or greater lock on the file
    ** already.
    **
    ** 对于预留锁或排它锁的请求，主要是基于本文件已经有一个共享锁
    ** 或者是更高一级的锁，
    */
    assert( 0!=pFile->eFileLock );
    lock.l_type = F_WRLCK;

    assert( eFileLock==RESERVED_LOCK || eFileLock==EXCLUSIVE_LOCK );
    if( eFileLock==RESERVED_LOCK ){
      lock.l_start = RESERVED_BYTE;
      lock.l_len = 1L;
    }else{
      lock.l_start = SHARED_FIRST;
      lock.l_len = SHARED_SIZE;
    }

    if( unixFileLock(pFile, &lock) ){
      tErrno = errno;
      rc = sqliteErrorFromPosixError(tErrno, SQLITE_IOERR_LOCK);
      if( rc!=SQLITE_BUSY ){
        pFile->lastErrno = tErrno;
      }
    }
  }
  

#ifdef SQLITE_DEBUG
  /* Set up the transaction-counter change checking flags when
  ** transitioning from a SHARED to a RESERVED lock.  The change
  ** from SHARED to RESERVED marks the beginning of a normal
  ** write operation (not a hot journal rollback).
  **
  ** 从共享锁转变为预留锁时，设置事务计数器更改检查标志。
  ** 这个改变标志着正常的写操作开始(而不是热日志回滚)
  */
  if( rc==SQLITE_OK
   && pFile->eFileLock<=SHARED_LOCK
   && eFileLock==RESERVED_LOCK
  ){
    pFile->transCntrChng = 0;
    pFile->dbUpdate = 0;
    pFile->inNormalWrite = 1;
  }
#endif


  if( rc==SQLITE_OK ){
    pFile->eFileLock = eFileLock;
    pInode->eFileLock = eFileLock;
  }else if( eFileLock==EXCLUSIVE_LOCK ){
    pFile->eFileLock = PENDING_LOCK;
    pInode->eFileLock = PENDING_LOCK;
  }

end_lock:
  unixLeaveMutex();
  OSTRACE(("LOCK    %d %s %s (unix)\n", pFile->h, azFileLock(eFileLock), 
      rc==SQLITE_OK ? "ok" : "failed"));
  return rc;
}

/*
** Add the file descriptor used by file handle pFile to the corresponding
** pUnused list.
**
** 由文件句柄pFile增加新的文件描述符到pUnused对象列表，
*/
static void setPendingFd(unixFile *pFile){
  unixInodeInfo *pInode = pFile->pInode;
  UnixUnusedFd *p = pFile->pUnused;
  p->pNext = pInode->pUnused;
  pInode->pUnused = p;
  pFile->h = -1;
  pFile->pUnused = 0;
}

/*
** Lower the locking level on file descriptor pFile to eFileLock.  eFileLock
** must be either NO_LOCK or SHARED_LOCK.
**
** If the locking level of the file descriptor is already at or below
** the requested locking level, this routine is a no-op.
** 
** If handleNFSUnlock is true, then on downgrading an EXCLUSIVE_LOCK to SHARED
** the byte range is divided into 2 parts and the first part is unlocked then
** set to a read lock, then the other part is simply unlocked.  This works 
** around a bug in BSD NFS lockd (also seen on MacOSX 10.3+) that fails to 
** remove the write lock on a region when a read lock is set.
** 
** 将文件描述符降低到eFileLock级别，必须是NO_LOCK或者
** SHARED_LOCK，
** 如果文件描述符的锁定级别已经是或者低于请求的
** 锁级别，那么本操作为空
** 如果NFSUnlock句柄为真，然后将EXCLUSIVE_LOCK降低为共享级别，
** 锁定字节分为两个部分，前一个解锁后一个设置为读锁。
** 这个工作主要是围绕BSD NFS的锁机制(包括MacOSX 10.3+)在读取
** 锁设置或删除设置的锁时发生失败
*/
static int posixUnlock(sqlite3_file *id, int eFileLock, int handleNFSUnlock){
  unixFile *pFile = (unixFile*)id;
  unixInodeInfo *pInode;
  struct flock lock;
  int rc = SQLITE_OK;

  assert( pFile );
  OSTRACE(("UNLOCK  %d %d was %d(%d,%d) pid=%d (unix)\n", pFile->h, eFileLock,
      pFile->eFileLock, pFile->pInode->eFileLock, pFile->pInode->nShared,
      getpid()));

  assert( eFileLock<=SHARED_LOCK );
  if( pFile->eFileLock<=eFileLock ){
    return SQLITE_OK;
  }
  unixEnterMutex();
  pInode = pFile->pInode;
  assert( pInode->nShared!=0 );
  if( pFile->eFileLock>SHARED_LOCK ){
    assert( pInode->eFileLock==pFile->eFileLock );

#ifdef SQLITE_DEBUG
    /* When reducing a lock such that other processes can start
    ** reading the database file again, make sure that the
    ** transaction counter was updated if any part of the database
    ** file changed.  If the transaction counter is not updated,
    ** other connections to the same file might not realize that
    ** the file has changed and hence might not know to flush their
    ** cache.  The use of a stale cache can lead to database corruption.
    ** 减少锁定以使	其他进程可以启动对于数据库文件的读写
    ** 确保事务计数器已更新。如果数据库的任何一部分文件得到
    ** 更新，但是事务计数器没有更新，其他连接到同一个文件的
    ** 进程可能不会发现文件已更改，也不知道需要更新缓存。使用
    ** 旧缓存会导致数据库损坏。
    */
    pFile->inNormalWrite = 0;
#endif

    /* downgrading to a shared lock on NFS involves clearing the write lock
    ** before establishing the readlock - to avoid a race condition we downgrade
    ** the lock in 2 blocks, so that part of the range will be covered by a 
    ** write lock until the rest is covered by a read lock:
    **  1:   [WWWWW]
    **  2:   [....W]
    **  3:   [RRRRW]
    **  4:   [RRRR.]
    **
    ** 在建立写锁前需要在NFS上共享锁的降级和写锁的清除，
    ** 从而避免在降级时发生两个区块间的竞争，所以其他部分
    ** 任然被写锁覆盖，直到被读锁覆盖:
    ** 1:[WWWWW]
    ** 2:[....W]
    ** 3:[RRRRRW]
    ** 4:[RRRR.]
    */
    if( eFileLock==SHARED_LOCK ){

#if !defined(__APPLE__) || !SQLITE_ENABLE_LOCKING_STYLE
      (void)handleNFSUnlock;
      assert( handleNFSUnlock==0 );
#endif
#if defined(__APPLE__) && SQLITE_ENABLE_LOCKING_STYLE
      if( handleNFSUnlock ){
        int tErrno;               /* Error code from system call errors */
        off_t divSize = SHARED_SIZE - 1;
        
        lock.l_type = F_UNLCK;
        lock.l_whence = SEEK_SET;
        lock.l_start = SHARED_FIRST;
        lock.l_len = divSize;
        if( unixFileLock(pFile, &lock)==(-1) ){
          tErrno = errno;
          rc = SQLITE_IOERR_UNLOCK;
          if( IS_LOCK_ERROR(rc) ){
            pFile->lastErrno = tErrno;
          }
          goto end_unlock;
        }
        lock.l_type = F_RDLCK;
        lock.l_whence = SEEK_SET;
        lock.l_start = SHARED_FIRST;
        lock.l_len = divSize;
        if( unixFileLock(pFile, &lock)==(-1) ){
          tErrno = errno;
          rc = sqliteErrorFromPosixError(tErrno, SQLITE_IOERR_RDLOCK);
          if( IS_LOCK_ERROR(rc) ){
            pFile->lastErrno = tErrno;
          }
          goto end_unlock;
        }
        lock.l_type = F_UNLCK;
        lock.l_whence = SEEK_SET;
        lock.l_start = SHARED_FIRST+divSize;
        lock.l_len = SHARED_SIZE-divSize;
        if( unixFileLock(pFile, &lock)==(-1) ){
          tErrno = errno;
          rc = SQLITE_IOERR_UNLOCK;
          if( IS_LOCK_ERROR(rc) ){
            pFile->lastErrno = tErrno;
          }
          goto end_unlock;
        }
      }else
#endif /* defined(__APPLE__) && SQLITE_ENABLE_LOCKING_STYLE */
      {
        lock.l_type = F_RDLCK;
        lock.l_whence = SEEK_SET;
        lock.l_start = SHARED_FIRST;
        lock.l_len = SHARED_SIZE;
        if( unixFileLock(pFile, &lock) ){
          /* In theory, the call to unixFileLock() cannot fail because another
          ** process is holding an incompatible lock. If it does, this 
          ** indicates that the other process is not following the locking
          ** protocol. If this happens, return SQLITE_IOERR_RDLOCK. Returning
          ** SQLITE_BUSY would confuse the upper layer (in practice it causes 
          ** an assert to fail). 
          ** 
          ** 理论上，调用unixFileLock()不会导致失败，其他的进程拥有
          ** 不兼容锁，如果发生，也就表面其他的进程没有遵从锁协议
          ** 发生这种情况时，返回SQLITE_IOERR_RDLOCK，返回SQLITE_BUSY会在
          ** 上层导致混乱(实践中导致断言失败)
          */ 
          rc = SQLITE_IOERR_RDLOCK;
          pFile->lastErrno = errno;
          goto end_unlock;
        }
      }
    }
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = PENDING_BYTE;
    lock.l_len = 2L;  assert( PENDING_BYTE+1==RESERVED_BYTE );
    if( unixFileLock(pFile, &lock)==0 ){
      pInode->eFileLock = SHARED_LOCK;
    }else{
      rc = SQLITE_IOERR_UNLOCK;
      pFile->lastErrno = errno;
      goto end_unlock;
    }
  }
  if( eFileLock==NO_LOCK ){
    /* Decrement the shared lock counter.  Release the lock using an
    ** OS call only when all threads in this same process have released
    ** the lock.
    **
    ** 减少共享锁的计数器，只有在所有线程都属于同一个
    ** 进程释放锁时使用系统调用释放锁
    */
    pInode->nShared--;
    if( pInode->nShared==0 ){
      lock.l_type = F_UNLCK;
      lock.l_whence = SEEK_SET;
      lock.l_start = lock.l_len = 0L;
      if( unixFileLock(pFile, &lock)==0 ){
        pInode->eFileLock = NO_LOCK;
      }else{
        rc = SQLITE_IOERR_UNLOCK;
        pFile->lastErrno = errno;
        pInode->eFileLock = NO_LOCK;
        pFile->eFileLock = NO_LOCK;
      }
    }

    /* Decrement the count of locks against this same file.  When the
    ** count reaches zero, close any other file descriptors whose close
    ** was deferred because of outstanding locks.
    ** 
    ** 减少对于同一个文件的计数器，在计数器值为0时，
    ** 关闭所有因延时而导致的文件描述符
    */
    pInode->nLock--;
    assert( pInode->nLock>=0 );
    if( pInode->nLock==0 ){
      closePendingFds(pFile);
    }
  }

end_unlock:
  unixLeaveMutex();
  if( rc==SQLITE_OK ) pFile->eFileLock = eFileLock;
  return rc;
}

/*
** Lower the locking level on file descriptor pFile to eFileLock.  eFileLock
** must be either NO_LOCK or SHARED_LOCK.
**
** If the locking level of the file descriptor is already at or below
** the requested locking level, this routine is a no-op.
**
** 将文件描述符上的锁降低到eFileLock。必须是NO_LOCK或SHARED_LOCK
** 如果文件描述符的锁级别已经是或者低于请求的锁级别，操作无效。
*/
static int unixUnlock(sqlite3_file *id, int eFileLock){
  return posixUnlock(id, eFileLock, 0);
}

/*
** This function performs the parts of the "close file" operation 
** common to all locking schemes. It closes the directory and file
** handles, if they are valid, and sets all fields of the unixFile
** structure to 0.
**
** It is *not* necessary to hold the mutex when this routine is called,
** even on VxWorks.  A mutex will be acquired on VxWorks by the
** vxworksReleaseFileId() routine.
**
** 这个函数执行文件关闭对应的锁定方案，关闭目录和文件句柄
** 如果这些有效，同时设置unixFile所有字段为0
** 
** 调用这个例程时不需要保持互斥体，即使在VxWorks上也是这样的
** 由vxworksReleaseField()在VxWorks上获得互斥量。
*/
static int closeUnixFile(sqlite3_file *id){
  unixFile *pFile = (unixFile*)id;
  if( pFile->h>=0 ){
    robust_close(pFile, pFile->h, __LINE__);
    pFile->h = -1;
  }
#if OS_VXWORKS
  if( pFile->pId ){
    if( pFile->ctrlFlags & UNIXFILE_DELETE ){
      osUnlink(pFile->pId->zCanonicalName);
    }
    vxworksReleaseFileId(pFile->pId);
    pFile->pId = 0;
  }
#endif
  OSTRACE(("CLOSE   %-3d\n", pFile->h));
  OpenCounter(-1);
  sqlite3_free(pFile->pUnused);
  memset(pFile, 0, sizeof(unixFile));
  return SQLITE_OK;
}

/*
** Close a file. 关闭一个文件
*/
static int unixClose(sqlite3_file *id){
  int rc = SQLITE_OK;
  unixFile *pFile = (unixFile *)id;
  unixUnlock(id, NO_LOCK);
  unixEnterMutex();

  /* unixFile.pInode is always valid here. Otherwise, a different close
  ** routine (e.g. nolockClose()) would be called instead.
  ** unixFile.pInode在此处总是有效的。否则，就会使用
  ** 一个不同的关闭方式(例如nolockClose())
  */
  assert( pFile->pInode->nLock>0 || pFile->pInode->bProcessLock==0 );
  if( ALWAYS(pFile->pInode) && pFile->pInode->nLock ){
    /* If there are outstanding locks, do not actually close the file just
    ** yet because that would clear those locks.  Instead, add the file
    ** descriptor to pInode->pUnused list.  It will be automatically closed 
    ** when the last lock is cleared.
    **
    ** 如果仍有未完成的锁，则实际上不需要关闭文件，因为这样会清除那些锁
    ** 而需要添加文件描述符到pInode->pUnused对象列表中，在最后一个锁清除时
    ** 自动关闭文件。
    */
    setPendingFd(pFile);
  }
  releaseInodeInfo(pFile);
  rc = closeUnixFile(id);
  unixLeaveMutex();
  return rc;
}

/************** End of the posix advisory lock implementation *****************
******************************************************************************/

/******************************************************************************
****************************** No-op Locking **********************************
**
** Of the various locking implementations available, this is by far the
** simplest:  locking is ignored.  No attempt is made to lock the database
** file for reading or writing.
**
** This locking mode is appropriate for use on read-only databases
** (ex: databases that are burned into CD-ROM, for example.)  It can
** also be used if the application employs some external mechanism to
** prevent simultaneous access of the same database by two or more
** database connections.  But there is a serious risk of database
** corruption if this locking mode is used in situations where multiple
** database connections are accessing the same database file at the same
** time and one or more of those connections are writing.
**
** 各种可用锁协议的实现，这是最简单的实现:锁被忽略，对于
** 读或者写数据库都不锁定数据库.
** 这个锁定模式仅仅用于只读数据库(例如烧制到CD-ROM的数据库)
** 如果应用程序使用某些外部机制，也可以使用，防止同一个
** 数据库被两个或更多同时访问.但是这其中存在着严重的数据库
** 损坏风险，如果这种锁定模式用于多个数据库连接并指向同一个
** 数据库同时一个或多个写操作。
**
*/

static int nolockCheckReservedLock(sqlite3_file *NotUsed, int *pResOut){
  UNUSED_PARAMETER(NotUsed);
  *pResOut = 0;
  return SQLITE_OK;
}
static int nolockLock(sqlite3_file *NotUsed, int NotUsed2){
  UNUSED_PARAMETER2(NotUsed, NotUsed2);
  return SQLITE_OK;
}
static int nolockUnlock(sqlite3_file *NotUsed, int NotUsed2){
  UNUSED_PARAMETER2(NotUsed, NotUsed2);
  return SQLITE_OK;
}

/*
** Close the file.
*/
static int nolockClose(sqlite3_file *id) {
  return closeUnixFile(id);
}

/******************* End of the no-op lock implementation *********************
******************************************************************************/

/******************************************************************************
************************* Begin dot-file Locking ******************************
**
** The dotfile locking implementation uses the existance of separate lock
** files (really a directory) to control access to the database.  This works
** on just about every filesystem imaginable.  But there are serious downsides:
**
**    (1)  There is zero concurrency.  A single reader blocks all other
**         connections from reading or writing the database.
**
**    (2)  An application crash or power loss can leave stale lock files
**         sitting around that need to be cleared manually.
**
** Nevertheless, a dotlock is an appropriate locking mode for use if no
** other locking strategy is available.
**
** Dotfile locking works by creating a subdirectory in the same directory as
** the database and with the same name but with a ".lock" extension added.
** The existance of a lock directory implies an EXCLUSIVE lock.  All other
** lock types (SHARED, RESERVED, PENDING) are mapped into EXCLUSIVE.
**
** dot文件锁定主要使用文件分离锁实现(确实是一个目录)
** 实现对于一个数据库的访问控制。这个在所有的文件系统
** 中都是可以预想的，但是存在严重的缺点:
** 
** (1) 零并发，单个读进程限制其他读取和写入数据库的连接
** (2) 应用程序崩溃或者断电会导致过时的文件锁定，需要进行手动清除
**
** 然而，dot文件锁在没有其他锁策略可用时是合适的锁机制。
** 
** dot文件锁通过锁定同一目录下创建一个同名的子目录实现数据库同名
** 只是以".lock"结尾。这个锁意味着排它锁，所有其他类型的锁都被映射
** 为互斥锁(共享锁，预留锁，pending锁)
*/

/*
** The file suffix added to the data base filename in order to create the
** lock directory.
**
** 将文件后缀名添加到数据库文件名后便于创建锁目录
*/
#define DOTLOCK_SUFFIX ".lock"

/*
** This routine checks if there is a RESERVED lock held on the specified
** file by this or any other process. If such a lock is held, set *pResOut
** to a non-zero value otherwise *pResOut is set to zero.  The return value
** is set to SQLITE_OK unless an I/O error occurs during lock checking.
**
** In dotfile locking, either a lock exists or it does not.  So in this
** variation of CheckReservedLock(), *pResOut is set to true if any lock
** is held on the file and false if the file is unlocked.
**
** 这个例程主要是检查特定的文件是否由本进程或其他进程
** 设置了 预定锁，如果还保留，设置*pResOut为非零值否则为零
** 返回值为SQLITE_OK，除非在进行锁定检查时发生IO错误
** 
** 在dot文件锁定中，无论锁存在还是不存在,需要在CheckReservedLock()
** 中进行改变，*pResOut在文件解锁时设置为假否则为真。
*/
static int dotlockCheckReservedLock(sqlite3_file *id, int *pResOut) {
  int rc = SQLITE_OK;
  int reserved = 0;
  unixFile *pFile = (unixFile*)id;

  SimulateIOError( return SQLITE_IOERR_CHECKRESERVEDLOCK; );
  
  assert( pFile );

  /* Check if a thread in this process holds such a lock */
  if( pFile->eFileLock>SHARED_LOCK ){
    /* Either this connection or some other connection in the same process
    ** holds a lock on the file.  No need to check further. */
    reserved = 1;
  }else{
    /* The lock is held if and only if the lockfile exists */
    const char *zLockFile = (const char*)pFile->lockingContext;
    reserved = osAccess(zLockFile, 0)==0;
  }
  OSTRACE(("TEST WR-LOCK %d %d %d (dotlock)\n", pFile->h, rc, reserved));
  *pResOut = reserved;
  return rc;
}

/*
** Lock the file with the lock specified by parameter eFileLock - one
** of the following:
**
**     (1) SHARED_LOCK
**     (2) RESERVED_LOCK
**     (3) PENDING_LOCK
**     (4) EXCLUSIVE_LOCK
**
** Sometimes when requesting one lock state, additional lock states
** are inserted in between.  The locking might fail on one of the later
** transitions leaving the lock state different from what it started but
** still short of its goal.  The following chart shows the allowed
** transitions and the inserted intermediate states:
**
**    UNLOCKED -> SHARED
**    SHARED -> RESERVED
**    SHARED -> (PENDING) -> EXCLUSIVE
**    RESERVED -> (PENDING) -> EXCLUSIVE
**    PENDING -> EXCLUSIVE
**
** This routine will only increase a lock.  Use the sqlite3OsUnlock()
** routine to lower a locking level.
**
** With dotfile locking, we really only support state (4): EXCLUSIVE.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       



** But we track the other locking levels internally.
**
** 在dot文件锁中，我们只要支持状态(4),独占，但是我们
** 在程序内部跟踪其他的锁定级别。
*/
static int dotlockLock(sqlite3_file *id, int eFileLock) {
  unixFile *pFile = (unixFile*)id;
  char *zLockFile = (char *)pFile->lockingContext;
  int rc = SQLITE_OK;


  /* If we have any lock, then the lock file already exists.  All we have
  ** to do is adjust our internal record of the lock level.
  ** 
  ** 如果我们拥有任何锁，而且锁文件存在，
  ** 我们需要调整锁的级别记录.
  */
  if( pFile->eFileLock > NO_LOCK ){
    pFile->eFileLock = eFileLock;
    /* Always update the timestamp on the old file 
	** 始终更新旧文件的时间戳
	*/
#ifdef HAVE_UTIME
    utime(zLockFile, NULL);
#else
    utimes(zLockFile, NULL);
#endif
    return SQLITE_OK;
  }
  
  /* grab an exclusive lock */
  rc = osMkdir(zLockFile, 0777);
  if( rc<0 ){
    /* failed to open/create the lock directory */
    int tErrno = errno;
    if( EEXIST == tErrno ){
      rc = SQLITE_BUSY;
    } else {
      rc = sqliteErrorFromPosixError(tErrno, SQLITE_IOERR_LOCK);
      if( IS_LOCK_ERROR(rc) ){
        pFile->lastErrno = tErrno;
      }
    }
    return rc;
  } 
  
  /* got it, set the type and return ok */
  pFile->eFileLock = eFileLock;
  return rc;
}

/*
** Lower the locking level on file descriptor pFile to eFileLock.  eFileLock
** must be either NO_LOCK or SHARED_LOCK.
**
** If the locking level of the file descriptor is already at or below
** the requested locking level, this routine is a no-op.
**
** When the locking level reaches NO_LOCK, delete the lock file.
**
** 将文件描述符在pFile 上的级别调整为eFileLock . eFileLock
** 必须为NO_LOCK 或SHARED_LOCK
** 如果文件描述符锁定级别等于或低于请求的锁定级别，操作无意义
** 当锁定级别达到NO_LOCK时，删除锁定文件
*/
static int dotlockUnlock(sqlite3_file *id, int eFileLock) {
  unixFile *pFile = (unixFile*)id;
  char *zLockFile = (char *)pFile->lockingContext;
  int rc;

  assert( pFile );
  OSTRACE(("UNLOCK  %d %d was %d pid=%d (dotlock)\n", pFile->h, eFileLock,
           pFile->eFileLock, getpid()));
  assert( eFileLock<=SHARED_LOCK );
  
  /* no-op if possible */
  if( pFile->eFileLock==eFileLock ){
    return SQLITE_OK;
  }

  /* To downgrade to shared, simply update our internal notion of the
  ** lock state.  No need to mess with the file on disk.
  */
  if( eFileLock==SHARED_LOCK ){
    pFile->eFileLock = SHARED_LOCK;
    return SQLITE_OK;
  }
  
  /* To fully unlock the database, delete the lock file */
  assert( eFileLock==NO_LOCK );
  rc = osRmdir(zLockFile);
  if( rc<0 && errno==ENOTDIR ) rc = osUnlink(zLockFile);
  if( rc<0 ){
    int tErrno = errno;
    rc = 0;
    if( ENOENT != tErrno ){
      rc = SQLITE_IOERR_UNLOCK;
    }
    if( IS_LOCK_ERROR(rc) ){
      pFile->lastErrno = tErrno;
    }
    return rc; 
  }
  pFile->eFileLock = NO_LOCK;
  return SQLITE_OK;
}

/*
** Close a file.  Make sure the lock has been released before closing.
*/
static int dotlockClose(sqlite3_file *id) {
  int rc;
  if( id ){
    unixFile *pFile = (unixFile*)id;
    dotlockUnlock(id, NO_LOCK);
    sqlite3_free(pFile->lockingContext);
  }
  rc = closeUnixFile(id);
  return rc;
}
/****************** End of the dot-file lock implementation *******************
******************************************************************************/

/******************************************************************************
************************** Begin flock Locking ********************************
**
** Use the flock() system call to do file locking.
**
** flock() locking is like dot-file locking in that the various
** fine-grain locking levels supported by SQLite are collapsed into
** a single exclusive lock.  In other words, SHARED, RESERVED, and
** PENDING locks are the same thing as an EXCLUSIVE lock.  SQLite
** still works when you do this, but concurrency is reduced since
** only a single process can be reading the database at a time.
**
** Omit this section if SQLITE_ENABLE_LOCKING_STYLE is turned off or if
** compiling for VXWORKS.
** 
** 							锁定整个文件
** 
** 使用flock()系统调用进行整个文件锁定
** flock()锁定就像dot文件锁定一样将SQLite支持的各种锁映射为单一的排他锁
** 换句话说，共享锁，预留锁和延迟锁在此时和互斥锁一样，SQLite在此时
** 仍然像你预期的那样工作，但是并发性降低，每次只有一个进程对数据库读写
** 如果SQLITE_ENABLE_LOCKING_STYLE关闭或者编译VXWORKS时忽略此部分
*/
#if SQLITE_ENABLE_LOCKING_STYLE && !OS_VXWORKS

/*
** Retry flock() calls that fail with EINTR
*/
#ifdef EINTR
static int robust_flock(int fd, int op){
  int rc;
  do{ rc = flock(fd,op); }while( rc<0 && errno==EINTR );
  return rc;
}
#else
# define robust_flock(a,b) flock(a,b)
#endif
     

/*
** This routine checks if there is a RESERVED lock held on the specified
** file by this or any other process. If such a lock is held, set *pResOut
** to a non-zero value otherwise *pResOut is set to zero.  The return value
** is set to SQLITE_OK unless an I/O error occurs during lock checking.
*/
static int flockCheckReservedLock(sqlite3_file *id, int *pResOut){
  int rc = SQLITE_OK;
  int reserved = 0;
  unixFile *pFile = (unixFile*)id;
  
  SimulateIOError( return SQLITE_IOERR_CHECKRESERVEDLOCK; );
  
  assert( pFile );
  
  /* Check if a thread in this process holds such a lock */
  if( pFile->eFileLock>SHARED_LOCK ){
    reserved = 1;
  }
  
  /* Otherwise see if some other process holds it. */
  if( !reserved ){
    /* attempt to get the lock */
    int lrc = robust_flock(pFile->h, LOCK_EX | LOCK_NB);
    if( !lrc ){
      /* got the lock, unlock it */
      lrc = robust_flock(pFile->h, LOCK_UN);
      if ( lrc ) {
        int tErrno = errno;
        /* unlock failed with an error */
        lrc = SQLITE_IOERR_UNLOCK; 
        if( IS_LOCK_ERROR(lrc) ){
          pFile->lastErrno = tErrno;
          rc = lrc;
        }
      }
    } else {
      int tErrno = errno;
      reserved = 1;
      /* someone else might have it reserved */
      lrc = sqliteErrorFromPosixError(tErrno, SQLITE_IOERR_LOCK); 
      if( IS_LOCK_ERROR(lrc) ){
        pFile->lastErrno = tErrno;
        rc = lrc;
      }
    }
  }
  OSTRACE(("TEST WR-LOCK %d %d %d (flock)\n", pFile->h, rc, reserved));

#ifdef SQLITE_IGNORE_FLOCK_LOCK_ERRORS
  if( (rc & SQLITE_IOERR) == SQLITE_IOERR ){
    rc = SQLITE_OK;
    reserved=1;
  }
#endif /* SQLITE_IGNORE_FLOCK_LOCK_ERRORS */
  *pResOut = reserved;
  return rc;
}

/*
** Lock the file with the lock specified by parameter eFileLock - one
** of the following:
**
**     (1) SHARED_LOCK
**     (2) RESERVED_LOCK
**     (3) PENDING_LOCK
**     (4) EXCLUSIVE_LOCK
**
** Sometimes when requesting one lock state, additional lock states
** are inserted in between.  The locking might fail on one of the later
** transitions leaving the lock state different from what it started but
** still short of its goal.  The following chart shows the allowed
** transitions and the inserted intermediate states:
**
**    UNLOCKED -> SHARED
**    SHARED -> RESERVED
**    SHARED -> (PENDING) -> EXCLUSIVE
**    RESERVED -> (PENDING) -> EXCLUSIVE
**    PENDING -> EXCLUSIVE
**
** flock() only really support EXCLUSIVE locks.  We track intermediate
** lock states in the sqlite3_file structure, but all locks SHARED or
** above are really EXCLUSIVE locks and exclude all other processes from
** access the file.
**
** This routine will only increase a lock.  Use the sqlite3OsUnlock()
** routine to lower a locking level.
**
** flock()只支持排它锁，我们在追踪sqlite3_file结构的中间状态时，发现
** 所有共享及之上的锁都表现为排它锁，不允许其他进程访问本文件                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               


*/
static int flockLock(sqlite3_file *id, int eFileLock) {
  int rc = SQLITE_OK;
  unixFile *pFile = (unixFile*)id;

  assert( pFile );

  /* if we already have a lock, it is exclusive.  
  ** Just adjust level and punt on outta here. */
  if (pFile->eFileLock > NO_LOCK) {
    pFile->eFileLock = eFileLock;
    return SQLITE_OK;
  }
  
  /* grab an exclusive lock */
  
  if (robust_flock(pFile->h, LOCK_EX | LOCK_NB)) {
    int tErrno = errno;
    /* didn't get, must be busy */
    rc = sqliteErrorFromPosixError(tErrno, SQLITE_IOERR_LOCK);
    if( IS_LOCK_ERROR(rc) ){
      pFile->lastErrno = tErrno;
    }
  } else {
    /* got it, set the type and return ok */
    pFile->eFileLock = eFileLock;
  }
  OSTRACE(("LOCK    %d %s %s (flock)\n", pFile->h, azFileLock(eFileLock), 
           rc==SQLITE_OK ? "ok" : "failed"));
#ifdef SQLITE_IGNORE_FLOCK_LOCK_ERRORS
  if( (rc & SQLITE_IOERR) == SQLITE_IOERR ){
    rc = SQLITE_BUSY;
  }
#endif /* SQLITE_IGNORE_FLOCK_LOCK_ERRORS */
  return rc;
}


/*
** Lower the locking level on file descriptor pFile to eFileLock.  eFileLock
** must be either NO_LOCK or SHARED_LOCK.
**
** If the locking level of the file descriptor is already at or below
** the requested locking level, this routine is a no-op.
*/
static int flockUnlock(sqlite3_file *id, int eFileLock) {
  unixFile *pFile = (unixFile*)id;
  
  assert( pFile );
  OSTRACE(("UNLOCK  %d %d was %d pid=%d (flock)\n", pFile->h, eFileLock,
           pFile->eFileLock, getpid()));
  assert( eFileLock<=SHARED_LOCK );
  
  /* no-op if possible */
  if( pFile->eFileLock==eFileLock ){
    return SQLITE_OK;
  }
  
  /* shared can just be set because we always have an exclusive */
  if (eFileLock==SHARED_LOCK) {
    pFile->eFileLock = eFileLock;
    return SQLITE_OK;
  }
  
  /* no, really, unlock. */
  if( robust_flock(pFile->h, LOCK_UN) ){
#ifdef SQLITE_IGNORE_FLOCK_LOCK_ERRORS
    return SQLITE_OK;
#endif /* SQLITE_IGNORE_FLOCK_LOCK_ERRORS */
    return SQLITE_IOERR_UNLOCK;
  }else{
    pFile->eFileLock = NO_LOCK;
    return SQLITE_OK;
  }
}

/*
** Close a file.
*/
static int flockClose(sqlite3_file *id) {
  if( id ){
    flockUnlock(id, NO_LOCK);
  }
  return closeUnixFile(id);
}

#endif /* SQLITE_ENABLE_LOCKING_STYLE && !OS_VXWORK */

/******************* End of the flock lock implementation *********************
******************************************************************************/

/******************************************************************************
************************ Begin Named Semaphore Locking ************************
**
** Named semaphore locking is only supported on VxWorks.
**
** Semaphore locking is like dot-lock and flock in that it really only
** supports EXCLUSIVE locking.  Only a single process can read or write
** the database file at a time.  This reduces potential concurrency, but
** makes the lock implementation much easier.
** 
** 						  标记信号量锁
** 标记信号量锁机制只在VxWorks上工作
** 信号量锁就像dot文件锁和全文件锁一样，只支持互斥锁。
** 只有一个进程在同一时间对文件进行读写操作。这降低了
** 潜在的并发操作，但是使得文件锁机制变得更加简单。
*/
#if OS_VXWORKS

/*
** This routine checks if there is a RESERVED lock held on the specified
** file by this or any other process. If such a lock is held, set *pResOut
** to a non-zero value otherwise *pResOut is set to zero.  The return value
** is set to SQLITE_OK unless an I/O error occurs during lock checking.
*/
static int semCheckReservedLock(sqlite3_file *id, int *pResOut) {
  int rc = SQLITE_OK;
  int reserved = 0;
  unixFile *pFile = (unixFile*)id;

  SimulateIOError( return SQLITE_IOERR_CHECKRESERVEDLOCK; );
  
  assert( pFile );

  /* Check if a thread in this process holds such a lock */
  if( pFile->eFileLock>SHARED_LOCK ){
    reserved = 1;
  }
  
  /* Otherwise see if some other process holds it. */
  if( !reserved ){
    sem_t *pSem = pFile->pInode->pSem;
    struct stat statBuf;

    if( sem_trywait(pSem)==-1 ){
      int tErrno = errno;
      if( EAGAIN != tErrno ){
        rc = sqliteErrorFromPosixError(tErrno, SQLITE_IOERR_CHECKRESERVEDLOCK);
        pFile->lastErrno = tErrno;
      } else {
        /* someone else has the lock when we are in NO_LOCK */
        reserved = (pFile->eFileLock < SHARED_LOCK);
      }
    }else{
      /* we could have it if we want it */
      sem_post(pSem);
    }
  }
  OSTRACE(("TEST WR-LOCK %d %d %d (sem)\n", pFile->h, rc, reserved));

  *pResOut = reserved;
  return rc;
}

/*
** Lock the file with the lock specified by parameter eFileLock - one
** of the following:
**
**     (1) SHARED_LOCK
**     (2) RESERVED_LOCK
**     (3) PENDING_LOCK
**     (4) EXCLUSIVE_LOCK
**
** Sometimes when requesting one lock state, additional lock states
** are inserted in between.  The locking might fail on one of the later
** transitions leaving the lock state different from what it started but
** still short of its goal.  The following chart shows the allowed
** transitions and the inserted intermediate states:
**
**    UNLOCKED -> SHARED
**    SHARED -> RESERVED
**    SHARED -> (PENDING) -> EXCLUSIVE
**    RESERVED -> (PENDING) -> EXCLUSIVE
**    PENDING -> EXCLUSIVE
**
** Semaphore locks only really support EXCLUSIVE locks.  We track intermediate
** lock states in the sqlite3_file structure, but all locks SHARED or
** above are really EXCLUSIVE locks and exclude all other processes from
** access the file.
**
** 信号量锁仅仅支持互斥锁，我们在sqlite3_file对象中追踪对应的锁状态
** 发现所有的共享锁及之上的锁实际上都是互斥锁，且禁止其他进程
** 对文件的访问。
**
** This routine will only increase a lock.  Use the sqlite3OsUnlock()
** routine to lower a locking level.
*/
static int semLock(sqlite3_file *id, int eFileLock) {
  unixFile *pFile = (unixFile*)id;
  int fd;
  sem_t *pSem = pFile->pInode->pSem;
  int rc = SQLITE_OK;

  /* if we already have a lock, it is exclusive.  
  ** Just adjust level and punt on outta here. */
  if (pFile->eFileLock > NO_LOCK) {
    pFile->eFileLock = eFileLock;
    rc = SQLITE_OK;
    goto sem_end_lock;
  }
  
  /* lock semaphore now but bail out when already locked. */
  if( sem_trywait(pSem)==-1 ){
    rc = SQLITE_BUSY;
    goto sem_end_lock;
  }

  /* got it, set the type and return ok */
  pFile->eFileLock = eFileLock;

 sem_end_lock:
  return rc;
}

/*
** Lower the locking level on file descriptor pFile to eFileLock.  eFileLock
** must be either NO_LOCK or SHARED_LOCK.
**
** If the locking level of the file descriptor is already at or below
** the requested locking level, this routine is a no-op.
*/
static int semUnlock(sqlite3_file *id, int eFileLock) {
  unixFile *pFile = (unixFile*)id;
  sem_t *pSem = pFile->pInode->pSem;

  assert( pFile );
  assert( pSem );
  OSTRACE(("UNLOCK  %d %d was %d pid=%d (sem)\n", pFile->h, eFileLock,
           pFile->eFileLock, getpid()));
  assert( eFileLock<=SHARED_LOCK );
  
  /* no-op if possible */
  if( pFile->eFileLock==eFileLock ){
    return SQLITE_OK;
  }
  
  /* shared can just be set because we always have an exclusive */
  if (eFileLock==SHARED_LOCK) {
    pFile->eFileLock = eFileLock;
    return SQLITE_OK;
  }
  
  /* no, really unlock. */
  if ( sem_post(pSem)==-1 ) {
    int rc, tErrno = errno;
    rc = sqliteErrorFromPosixError(tErrno, SQLITE_IOERR_UNLOCK);
    if( IS_LOCK_ERROR(rc) ){
      pFile->lastErrno = tErrno;
    }
    return rc; 
  }
  pFile->eFileLock = NO_LOCK;
  return SQLITE_OK;
}

/*
 ** Close a file.
 */
static int semClose(sqlite3_file *id) {
  if( id ){
    unixFile *pFile = (unixFile*)id;
    semUnlock(id, NO_LOCK);
    assert( pFile );
    unixEnterMutex();
    releaseInodeInfo(pFile);
    unixLeaveMutex();
    closeUnixFile(id);
  }
  return SQLITE_OK;
}

#endif /* OS_VXWORKS */
/*
** Named semaphore locking is only available on VxWorks.
**
*************** End of the named semaphore lock implementation ****************
******************************************************************************/


/******************************************************************************
*************************** Begin AFP Locking *********************************
**
** AFP is the Apple Filing Protocol.  AFP is a network filesystem found
** on Apple Macintosh computers - both OS9 and OSX.
**
** Third-party implementations of AFP are available.  But this code here
** only works on OSX.
**                                             AFP锁
** AFP是Apple的文件保护机制，在Apple的macintosh计算机上为
** 网络文件系统，包括OS9和OSX，
**
** AFP的第三方实现可用，但此处的代码仅仅在OSX上工作。
*/

#if defined(__APPLE__) && SQLITE_ENABLE_LOCKING_STYLE
/*
** The afpLockingContext structure contains all afp lock specific state
*/
typedef struct afpLockingContext afpLockingContext;
struct afpLockingContext {
  int reserved;
  const char *dbPath;             /* Name of the open file */
};

struct ByteRangeLockPB2
{
  unsigned long long offset;        /* offset to first byte to lock */
  unsigned long long length;        /* nbr of bytes to lock */
  unsigned long long retRangeStart; /* nbr of 1st byte locked if successful */
  unsigned char unLockFlag;         /* 1 = unlock, 0 = lock */
  unsigned char startEndFlag;       /* 1=rel to end of fork, 0=rel to start */
  int fd;                           /* file desc to assoc this lock with */
};

#define afpfsByteRangeLock2FSCTL        _IOWR('z', 23, struct ByteRangeLockPB2)

/*
** This is a utility for setting or clearing a bit-range lock on an
** AFP filesystem.
** 
** Return SQLITE_OK on success, SQLITE_BUSY on failure.
** 
** 用于设置和清除AFP文件系统中的置位。
** 成功时返回SQLITE_OK,失败是返回SQLITE_BUSY
*/
static int afpSetLock(
  const char *path,              /* Name of the file to be locked or unlocked */
  unixFile *pFile,               /* Open file descriptor on path */
  unsigned long long offset,     /* First byte to be locked */
  unsigned long long length,     /* Number of bytes to lock */
  int setLockFlag                /* True to set lock.  False to clear lock */
){
  struct ByteRangeLockPB2 pb;
  int err;
  
  pb.unLockFlag = setLockFlag ? 0 : 1;
  pb.startEndFlag = 0;
  pb.offset = offset;
  pb.length = length; 
  pb.fd = pFile->h;
  
  OSTRACE(("AFPSETLOCK [%s] for %d%s in range %llx:%llx\n", 
    (setLockFlag?"ON":"OFF"), pFile->h, (pb.fd==-1?"[testval-1]":""),
    offset, length));
  err = fsctl(path, afpfsByteRangeLock2FSCTL, &pb, 0);
  if ( err==-1 ) {
    int rc;
    int tErrno = errno;
    OSTRACE(("AFPSETLOCK failed to fsctl() '%s' %d %s\n",
             path, tErrno, strerror(tErrno)));
#ifdef SQLITE_IGNORE_AFP_LOCK_ERRORS
    rc = SQLITE_BUSY;
#else
    rc = sqliteErrorFromPosixError(tErrno,
                    setLockFlag ? SQLITE_IOERR_LOCK : SQLITE_IOERR_UNLOCK);
#endif /* SQLITE_IGNORE_AFP_LOCK_ERRORS */
    if( IS_LOCK_ERROR(rc) ){
      pFile->lastErrno = tErrno;
    }
    return rc;
  } else {
    return SQLITE_OK;
  }
}

/*
** This routine checks if there is a RESERVED lock held on the specified
** file by this or any other process. If such a lock is held, set *pResOut
** to a non-zero value otherwise *pResOut is set to zero.  The return value
** is set to SQLITE_OK unless an I/O error occurs during lock checking.
*/
static int afpCheckReservedLock(sqlite3_file *id, int *pResOut){
  int rc = SQLITE_OK;
  int reserved = 0;
  unixFile *pFile = (unixFile*)id;
  afpLockingContext *context;
  
  SimulateIOError( return SQLITE_IOERR_CHECKRESERVEDLOCK; );
  
  assert( pFile );
  context = (afpLockingContext *) pFile->lockingContext;
  if( context->reserved ){
    *pResOut = 1;
    return SQLITE_OK;
  }
  unixEnterMutex(); /* Because pFile->pInode is shared across threads */
  
  /* Check if a thread in this process holds such a lock */
  if( pFile->pInode->eFileLock>SHARED_LOCK ){
    reserved = 1;
  }
  
  /* Otherwise see if some other process holds it.
   */
  if( !reserved ){
    /* lock the RESERVED byte */
    int lrc = afpSetLock(context->dbPath, pFile, RESERVED_BYTE, 1,1);  
    if( SQLITE_OK==lrc ){
      /* if we succeeded in taking the reserved lock, unlock it to restore
      ** the original state */
      lrc = afpSetLock(context->dbPath, pFile, RESERVED_BYTE, 1, 0);
    } else {
      /* if we failed to get the lock then someone else must have it */
      reserved = 1;
    }
    if( IS_LOCK_ERROR(lrc) ){
      rc=lrc;
    }
  }
  
  unixLeaveMutex();
  OSTRACE(("TEST WR-LOCK %d %d %d (afp)\n", pFile->h, rc, reserved));
  
  *pResOut = reserved;
  return rc;
}

/*
** Lock the file with the lock specified by parameter eFileLock - one
** of the following:
**
**     (1) SHARED_LOCK
**     (2) RESERVED_LOCK
**     (3) PENDING_LOCK
**     (4) EXCLUSIVE_LOCK
**
** Sometimes when requesting one lock state, additional lock states
** are inserted in between.  The locking might fail on one of the later
** transitions leaving the lock state different from what it started but
** still short of its goal.  The following chart shows the allowed
** transitions and the inserted intermediate states:
**
**    UNLOCKED -> SHARED
**    SHARED -> RESERVED
**    SHARED -> (PENDING) -> EXCLUSIVE
**    RESERVED -> (PENDING) -> EXCLUSIVE
**    PENDING -> EXCLUSIVE
**
** This routine will only increase a lock.  Use the sqlite3OsUnlock()
** routine to lower a locking level.
*/
static int afpLock(sqlite3_file *id, int eFileLock){
  int rc = SQLITE_OK;
  unixFile *pFile = (unixFile*)id;
  unixInodeInfo *pInode = pFile->pInode;
  afpLockingContext *context = (afpLockingContext *) pFile->lockingContext;
  
  assert( pFile );
  OSTRACE(("LOCK    %d %s was %s(%s,%d) pid=%d (afp)\n", pFile->h,
           azFileLock(eFileLock), azFileLock(pFile->eFileLock),
           azFileLock(pInode->eFileLock), pInode->nShared , getpid()));

  /* If there is already a lock of this type or more restrictive on the
  ** unixFile, do nothing. Don't use the afp_end_lock: exit path, as
  ** unixEnterMutex() hasn't been called yet.
  **
  ** 如果unixFile中已经存在这种或者更加严格的锁，则不产生动作，
  ** 不使用afp_end_lock:离开当前路径，就像unixEnterMutex()函数没有被调用。
  */
  if( pFile->eFileLock>=eFileLock ){
    OSTRACE(("LOCK    %d %s ok (already held) (afp)\n", pFile->h,
           azFileLock(eFileLock)));
    return SQLITE_OK;
  }

  /* Make sure the locking sequence is correct
  **  (1) We never move from unlocked to anything higher than shared lock.
  **  (2) SQLite never explicitly requests a pendig lock.
  **  (3) A shared lock is always held when a reserve lock is requested.
  */
  assert( pFile->eFileLock!=NO_LOCK || eFileLock==SHARED_LOCK );
  assert( eFileLock!=PENDING_LOCK );
  assert( eFileLock!=RESERVED_LOCK || pFile->eFileLock==SHARED_LOCK );
  
  /* This mutex is needed because pFile->pInode is shared across threads
  ** 再此需要互斥锁，因为pFile->pInode需要在线程间共享
  */
  unixEnterMutex();
  pInode = pFile->pInode;

  /* If some thread using this PID has a lock via a different unixFile*
  ** handle that precludes the requested lock, return BUSY.
  **
  ** 如果一些线程借助不同的unixFile指针排除所请求，返回BUSY
  */
  if( (pFile->eFileLock!=pInode->eFileLock && 
       (pInode->eFileLock>=PENDING_LOCK || eFileLock>SHARED_LOCK))
     ){
    rc = SQLITE_BUSY;
    goto afp_end_lock;
  }
  
  /* If a SHARED lock is requested, and some thread using this PID already
  ** has a SHARED or RESERVED lock, then increment reference counts and
  ** return SQLITE_OK.
  ** 如果请求一个共享锁，而且其他线程已经在使用这个PID占有
  ** 共享锁或是预留锁，然后增加引用计数并返回SQLITE_OK.
  */
  if( eFileLock==SHARED_LOCK && 
     (pInode->eFileLock==SHARED_LOCK || pInode->eFileLock==RESERVED_LOCK) ){
    assert( eFileLock==SHARED_LOCK );
    assert( pFile->eFileLock==0 );
    assert( pInode->nShared>0 );
    pFile->eFileLock = SHARED_LOCK;
    pInode->nShared++;
    pInode->nLock++;
    goto afp_end_lock;
  }
    
  /* A PENDING lock is needed before acquiring a SHARED lock and before
  ** acquiring an EXCLUSIVE lock.  For the SHARED lock, the PENDING will
  ** be released.
  */
  if( eFileLock==SHARED_LOCK 
      || (eFileLock==EXCLUSIVE_LOCK && pFile->eFileLock<PENDING_LOCK)
  ){
    int failed;
    failed = afpSetLock(context->dbPath, pFile, PENDING_BYTE, 1, 1);
    if (failed) {
      rc = failed;
      goto afp_end_lock;
    }
  }
  
  /* If control gets to this point, then actually go ahead and make
  ** operating system calls for the specified lock.
  */
  if( eFileLock==SHARED_LOCK ){
    int lrc1, lrc2, lrc1Errno = 0;
    long lk, mask;
    
    assert( pInode->nShared==0 );
    assert( pInode->eFileLock==0 );
        
    mask = (sizeof(long)==8) ? LARGEST_INT64 : 0x7fffffff;
    /* Now get the read-lock SHARED_LOCK */
    /* note that the quality of the randomness doesn't matter that much 
	**
	** 现在获取只读的共享锁
	** 随机性质量并不重要*/
    lk = random(); 
    pInode->sharedByte = (lk & mask)%(SHARED_SIZE - 1);
    lrc1 = afpSetLock(context->dbPath, pFile, 
          SHARED_FIRST+pInode->sharedByte, 1, 1);
    if( IS_LOCK_ERROR(lrc1) ){
      lrc1Errno = pFile->lastErrno;
    }
    /* Drop the temporary PENDING lock */
    lrc2 = afpSetLock(context->dbPath, pFile, PENDING_BYTE, 1, 0);
    
    if( IS_LOCK_ERROR(lrc1) ) {
      pFile->lastErrno = lrc1Errno;
      rc = lrc1;
      goto afp_end_lock;
    } else if( IS_LOCK_ERROR(lrc2) ){
      rc = lrc2;
      goto afp_end_lock;
    } else if( lrc1 != SQLITE_OK ) {
      rc = lrc1;
    } else {
      pFile->eFileLock = SHARED_LOCK;
      pInode->nLock++;
      pInode->nShared = 1;
    }
  }else if( eFileLock==EXCLUSIVE_LOCK && pInode->nShared>1 ){
    /* We are trying for an exclusive lock but another thread in this
     ** same process is still holding a shared lock. */
    rc = SQLITE_BUSY;
  }else{
    /* The request was for a RESERVED or EXCLUSIVE lock.  It is
    ** assumed that there is a SHARED or greater lock on the file
    ** already.
    */
    int failed = 0;
    assert( 0!=pFile->eFileLock );
    if (eFileLock >= RESERVED_LOCK && pFile->eFileLock < RESERVED_LOCK) {
        /* Acquire a RESERVED lock */
        failed = afpSetLock(context->dbPath, pFile, RESERVED_BYTE, 1,1);
      if( !failed ){
        context->reserved = 1;
      }
    }
    if (!failed && eFileLock == EXCLUSIVE_LOCK) {
      /* Acquire an EXCLUSIVE lock */
        
      /* Remove the shared lock before trying the range.  we'll need to 
      ** reestablish the shared lock if we can't get the  afpUnlock
      */
      if( !(failed = afpSetLock(context->dbPath, pFile, SHARED_FIRST +
                         pInode->sharedByte, 1, 0)) ){
        int failed2 = SQLITE_OK;
        /* now attemmpt to get the exclusive lock range */
        failed = afpSetLock(context->dbPath, pFile, SHARED_FIRST, 
                               SHARED_SIZE, 1);
        if( failed && (failed2 = afpSetLock(context->dbPath, pFile, 
                       SHARED_FIRST + pInode->sharedByte, 1, 1)) ){
          /* Can't reestablish the shared lock.  Sqlite can't deal, this is
          ** a critical I/O error
          */
          rc = ((failed & SQLITE_IOERR) == SQLITE_IOERR) ? failed2 : 
               SQLITE_IOERR_LOCK;
          goto afp_end_lock;
        } 
      }else{
        rc = failed; 
      }
    }
    if( failed ){
      rc = failed;
    }
  }
  
  if( rc==SQLITE_OK ){
    pFile->eFileLock = eFileLock;
    pInode->eFileLock = eFileLock;
  }else if( eFileLock==EXCLUSIVE_LOCK ){
    pFile->eFileLock = PENDING_LOCK;
    pInode->eFileLock = PENDING_LOCK;
  }
  
afp_end_lock:
  unixLeaveMutex();
  OSTRACE(("LOCK    %d %s %s (afp)\n", pFile->h, azFileLock(eFileLock), 
         rc==SQLITE_OK ? "ok" : "failed"));
  return rc;
}

/*
** Lower the locking level on file descriptor pFile to eFileLock.  eFileLock
** must be either NO_LOCK or SHARED_LOCK.
**
** If the locking level of the file descriptor is already at or below
** the requested locking level, this routine is a no-op.
*/
static int afpUnlock(sqlite3_file *id, int eFileLock) {
  int rc = SQLITE_OK;
  unixFile *pFile = (unixFile*)id;
  unixInodeInfo *pInode;
  afpLockingContext *context = (afpLockingContext *) pFile->lockingContext;
  int skipShared = 0;
#ifdef SQLITE_TEST
  int h = pFile->h;
#endif

  assert( pFile );
  OSTRACE(("UNLOCK  %d %d was %d(%d,%d) pid=%d (afp)\n", pFile->h, eFileLock,
           pFile->eFileLock, pFile->pInode->eFileLock, pFile->pInode->nShared,
           getpid()));

  assert( eFileLock<=SHARED_LOCK );
  if( pFile->eFileLock<=eFileLock ){
    return SQLITE_OK;
  }
  unixEnterMutex();
  pInode = pFile->pInode;
  assert( pInode->nShared!=0 );
  if( pFile->eFileLock>SHARED_LOCK ){
    assert( pInode->eFileLock==pFile->eFileLock );
    SimulateIOErrorBenign(1);
    SimulateIOError( h=(-1) )
    SimulateIOErrorBenign(0);
    
#ifdef SQLITE_DEBUG
    /* When reducing a lock such that other processes can start
    ** reading the database file again, make sure that the                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  


    ** transaction counter was updated if any part of the database
    ** file changed.  If the transaction counter is not updated,
    ** other connections to the same file might not realize that
    ** the file has changed and hence might not know to flush their
    ** cache.  The use of a stale cache can lead to database corruption.
    */
    assert( pFile->inNormalWrite==0
           || pFile->dbUpdate==0
           || pFile->transCntrChng==1 );
    pFile->inNormalWrite = 0;
#endif
    
    if( pFile->eFileLock==EXCLUSIVE_LOCK ){
      rc = afpSetLock(context->dbPath, pFile, SHARED_FIRST, SHARED_SIZE, 0);
      if( rc==SQLITE_OK && (eFileLock==SHARED_LOCK || pInode->nShared>1) ){
        /* only re-establish the shared lock if necessary */
        int sharedLockByte = SHARED_FIRST+pInode->sharedByte;
        rc = afpSetLock(context->dbPath, pFile, sharedLockByte, 1, 1);
      } else {
        skipShared = 1;
      }
    }
    if( rc==SQLITE_OK && pFile->eFileLock>=PENDING_LOCK ){
      rc = afpSetLock(context->dbPath, pFile, PENDING_BYTE, 1, 0);
    } 
    if( rc==SQLITE_OK && pFile->eFileLock>=RESERVED_LOCK && context->reserved ){
      rc = afpSetLock(context->dbPath, pFile, RESERVED_BYTE, 1, 0);
      if( !rc ){ 
        context->reserved = 0; 
      }
    }
    if( rc==SQLITE_OK && (eFileLock==SHARED_LOCK || pInode->nShared>1)){
      pInode->eFileLock = SHARED_LOCK;
    }
  }
  if( rc==SQLITE_OK && eFileLock==NO_LOCK ){

    /* Decrement the shared lock counter.  Release the lock using an
    ** OS call only when all threads in this same process have released
    ** the lock.
    */
    unsigned long long sharedLockByte = SHARED_FIRST+pInode->sharedByte;
    pInode->nShared--;
    if( pInode->nShared==0 ){
      SimulateIOErrorBenign(1);
      SimulateIOError( h=(-1) )
      SimulateIOErrorBenign(0);
      if( !skipShared ){
        rc = afpSetLock(context->dbPath, pFile, sharedLockByte, 1, 0);
      }
      if( !rc ){
        pInode->eFileLock = NO_LOCK;
        pFile->eFileLock = NO_LOCK;
      }
    }
    if( rc==SQLITE_OK ){
      pInode->nLock--;
      assert( pInode->nLock>=0 );
      if( pInode->nLock==0 ){
        closePendingFds(pFile);
      }
    }
  }
  
  unixLeaveMutex();
  if( rc==SQLITE_OK ) pFile->eFileLock = eFileLock;
  return rc;
}

/*
** Close a file & cleanup AFP specific locking context 
** 关闭文件，清除所有的AFP特殊锁结构
*/
static int afpClose(sqlite3_file *id) {
  int rc = SQLITE_OK;
  if( id ){
    unixFile *pFile = (unixFile*)id;
    afpUnlock(id, NO_LOCK);
    unixEnterMutex();
    if( pFile->pInode && pFile->pInode->nLock ){
      /* If there are outstanding locks, do not actually close the file just
      ** yet because that would clear those locks.  Instead, add the file
      ** descriptor to pInode->aPending.  It will be automatically closed when
      ** the last lock is cleared.
      */
      setPendingFd(pFile);
    }
    releaseInodeInfo(pFile);
    sqlite3_free(pFile->lockingContext);
    rc = closeUnixFile(id);
    unixLeaveMutex();
  }
  return rc;
}

#endif /* defined(__APPLE__) && SQLITE_ENABLE_LOCKING_STYLE */
/*
** The code above is the AFP lock implementation.  The code is specific
** to MacOSX and does not work on other unix platforms.  No alternative
** is available.  If you don't compile for a mac, then the "unix-afp"
** VFS is not available.
**
** 以上的代码主要是AFP锁机制的实现，	是为MacOSX设计，不能
** 在其他的unix平台上工作，没有其他可以选择的方式。如果你不
** 是为mac编译，然后"unix-afp"选项就不用选择。
**
********************* End of the AFP lock implementation **********************
******************************************************************************/

/******************************************************************************
*************************** Begin NFS Locking *******************************
**
**                                                NFS锁实现
**
*/

#if defined(__APPLE__) && SQLITE_ENABLE_LOCKING_STYLE
/*
 ** Lower the locking level on file descriptor pFile to eFileLock.  eFileLock
 ** must be either NO_LOCK or SHARED_LOCK.
 **
 ** If the locking level of the file descriptor is already at or below
 ** the requested locking level, this routine is a no-op.
 */
static int nfsUnlock(sqlite3_file *id, int eFileLock){
  return posixUnlock(id, eFileLock, 1);
}

#endif /* defined(__APPLE__) && SQLITE_ENABLE_LOCKING_STYLE */
/*
** The code above is the NFS lock implementation.  The code is specific
** to MacOSX and does not work on other unix platforms.  No alternative
** is available.  
**
** 以上的代码就是NFS锁实现，主要是为MacOSX服务，
** 不能在其他的unix平台工作，也没有其他可以选择的方法
**
********************* End of the NFS lock implementation **********************
******************************************************************************/

/******************************************************************************
**************** Non-locking sqlite3_file methods *****************************
**  
**                              sqlite3_file非锁定方法
**
** The next division contains implementations for all methods of the 
** sqlite3_file object other than the locking methods.  The locking
** methods were defined in divisions above (one locking method per
** division).  Those methods that are common to all locking modes
** are gather together into this division.
**
** 接下来的这个部分包含了sqlite3_file对象除了锁定处理之外的操作，
** 锁定的方法主要是在此前的部分定义(每种锁定方式都是单独一个
** 部分)。这些方法中公共部分组成了这一节。
*/

/*
** Seek to the offset passed as the second argument, then read cnt 
** bytes into pBuf. Return the number of bytes actually read.
**
** NB:  If you define USE_PREAD or USE_PREAD64, then it might also
** be necessary to define _XOPEN_SOURCE to be 500.  This varies from
** one system to another.  Since SQLite does not define USE_PREAD
** any any form by default, we will not attempt to define _XOPEN_SOURCE.
** See tickets #2741 and #2681.
**
** To avoid stomping the errno value on a failed read the lastErrno value
** is set before returning.
**
** 寻找第二个参数偏移地址的传递值，然后将读取的cnt字节转换为pBuf
** 对象，返回实际读取的字节数。
** 
** 注意:如果你有定义USE_PREAD或USE_PREAD64，那么必须定义_XOPEN_SOURCE为500
** 这个在不同系统之间不同。因为SQLite在任何情况下都没有定义USE_PREAD
** 我们以后也不会定义_XOPEN_SOURCE，详情见#2741和@2681
**
** 为了避免存储读错误时的错误值，在存储前需要存储最后一个错误值
*/
static int seekAndRead(unixFile *id, sqlite3_int64 offset, void *pBuf, int cnt){
  int got;
  int prior = 0;
#if (!defined(USE_PREAD) && !defined(USE_PREAD64))
  i64 newOffset;
#endif
  TIMER_START;
  assert( cnt==(cnt&0x1ffff) );
  cnt &= 0x1ffff;
  do{
#if defined(USE_PREAD)
    got = osPread(id->h, pBuf, cnt, offset);
    SimulateIOError( got = -1 );
#elif defined(USE_PREAD64)
    got = osPread64(id->h, pBuf, cnt, offset);
    SimulateIOError( got = -1 );
#else
    newOffset = lseek(id->h, offset, SEEK_SET);
    SimulateIOError( newOffset-- );
    if( newOffset!=offset ){
      if( newOffset == -1 ){
        ((unixFile*)id)->lastErrno = errno;
      }else{
        ((unixFile*)id)->lastErrno = 0;
      }
      return -1;
    }
    got = osRead(id->h, pBuf, cnt);
#endif
    if( got==cnt ) break;
    if( got<0 ){
      if( errno==EINTR ){ got = 1; continue; }
      prior = 0;
      ((unixFile*)id)->lastErrno = errno;
      break;
    }else if( got>0 ){
      cnt -= got;
      offset += got;
      prior += got;
      pBuf = (void*)(got + (char*)pBuf);
    }
  }while( got>0 );
  TIMER_END;
  OSTRACE(("READ    %-3d %5d %7lld %llu\n",
            id->h, got+prior, offset-prior, TIMER_ELAPSED));
  return got+prior;
}

/*
** Read data from a file into a buffer.  Return SQLITE_OK if all
** bytes were read successfully and SQLITE_IOERR if anything goes
** wrong.
**
** 将文件中的数据读入缓冲区。 如果所有字节成功读取
** 返回SQLITE_OK，读取错误返回SQLITE_IOERR
**
*/
static int unixRead(
  sqlite3_file *id, 
  void *pBuf, 
  int amt,
  sqlite3_int64 offset
){
  unixFile *pFile = (unixFile *)id;
  int got;
  assert( id );

  /* If this is a database file (not a journal, master-journal or temp
  ** file), the bytes in the locking range should never be read or written. */
#if 0
  assert( pFile->pUnused==0
       || offset>=PENDING_BYTE+512
       || offset+amt<=PENDING_BYTE 
  );
#endif

  got = seekAndRead(pFile, offset, pBuf, amt);
  if( got==amt ){
    return SQLITE_OK;
  }else if( got<0 ){
    /* lastErrno set by seekAndRead */
    return SQLITE_IOERR_READ;
  }else{
    pFile->lastErrno = 0; /* not a system error */
    /* Unread parts of the buffer must be zero-filled */
    memset(&((char*)pBuf)[got], 0, amt-got);
    return SQLITE_IOERR_SHORT_READ;
  }
}

/*
** Seek to the offset in id->offset then read cnt bytes into pBuf.
** Return the number of bytes actually read.  Update the offset.
**
** To avoid stomping the errno value on a failed write the lastErrno value
** is set before returning.
**
**在id-> offset中寻找偏移量，然后将cnt个字节读入pBuf。 
**返回实际读取的字节数。 更新偏移量。
**
**为了避免在写入失败时破坏errno值，在返回之前设置lastErrno值。
**
*/
static int seekAndWrite(unixFile *id, i64 offset, const void *pBuf, int cnt){
  int got;
#if (!defined(USE_PREAD) && !defined(USE_PREAD64))
  i64 newOffset;
#endif
  assert( cnt==(cnt&0x1ffff) );
  cnt &= 0x1ffff;
  TIMER_START;
#if defined(USE_PREAD)
  do{ got = osPwrite(id->h, pBuf, cnt, offset); }while( got<0 && errno==EINTR );
#elif defined(USE_PREAD64)
  do{ got = osPwrite64(id->h, pBuf, cnt, offset);}while( got<0 && errno==EINTR);
#else
  do{
    newOffset = lseek(id->h, offset, SEEK_SET);
    SimulateIOError( newOffset-- );
    if( newOffset!=offset ){
      if( newOffset == -1 ){
        ((unixFile*)id)->lastErrno = errno;
      }else{
        ((unixFile*)id)->lastErrno = 0;
      }
      return -1;
    }
    got = osWrite(id->h, pBuf, cnt);
  }while( got<0 && errno==EINTR );
#endif
  TIMER_END;
  if( got<0 ){
    ((unixFile*)id)->lastErrno = errno;
  }

  OSTRACE(("WRITE   %-3d %5d %7lld %llu\n", id->h, got, offset, TIMER_ELAPSED));
  return got;
}


/*
** Write data from a buffer into a file.  Return SQLITE_OK on success
** or some other error code on failure.
*/
static int unixWrite(
  sqlite3_file *id, 
  const void *pBuf, 
  int amt,
  sqlite3_int64 offset 
){
  unixFile *pFile = (unixFile*)id;
  int wrote = 0;
  assert( id );
  assert( amt>0 );

  /* If this is a database file (not a journal, master-journal or temp
  ** file), the bytes in the locking range should never be read or written. */
#if 0
  assert( pFile->pUnused==0
       || offset>=PENDING_BYTE+512
       || offset+amt<=PENDING_BYTE 
  );
#endif

#ifdef SQLITE_DEBUG
  /* If we are doing a normal write to a database file (as opposed to
  ** doing a hot-journal rollback or a write to some file other than a
  ** normal database file) then record the fact that the database
  ** has changed.  If the transaction counter is modified, record that
  ** fact too.
  */
  if( pFile->inNormalWrite ){
    pFile->dbUpdate = 1;  /* The database has been modified */
    if( offset<=24 && offset+amt>=27 ){
      int rc;
      char oldCntr[4];
      SimulateIOErrorBenign(1);
      rc = seekAndRead(pFile, 24, oldCntr, 4);
      SimulateIOErrorBenign(0);
      if( rc!=4 || memcmp(oldCntr, &((char*)pBuf)[24-offset], 4)!=0 ){
        pFile->transCntrChng = 1;  /* The transaction counter has changed */
      }
    }
  }
#endif

  while( amt>0 && (wrote = seekAndWrite(pFile, offset, pBuf, amt))>0 ){
    amt -= wrote;
    offset += wrote;
    pBuf = &((char*)pBuf)[wrote];
  }
  SimulateIOError(( wrote=(-1), amt=1 ));
  SimulateDiskfullError(( wrote=0, amt=1 ));

  if( amt>0 ){
    if( wrote<0 && pFile->lastErrno!=ENOSPC ){
      /* lastErrno set by seekAndWrite */
      return SQLITE_IOERR_WRITE;
    }else{
      pFile->lastErrno = 0; /* not a system error */
      return SQLITE_FULL;
    }
  }

  return SQLITE_OK;
}

#ifdef SQLITE_TEST
/*
** Count the number of fullsyncs and normal syncs.  This is used to test
** that syncs and fullsyncs are occurring at the right times.
**
** 记录全同步和正常同步的次数，用于测试同步和全同步
** 正确进行的次数
** 
*/
int sqlite3_sync_count = 0;
int sqlite3_fullsync_count = 0;
#endif

/*
** We do not trust systems to provide a working fdatasync().  Some do.
** Others do no.  To be safe, we will stick with the (slightly slower)
** fsync(). If you know that your system does support fdatasync() correctly,
** then simply compile with -Dfdatasync=fdatasync
**
** 我们不信任系统提供的fdarasync()函数，有些数据库认为可以
** 但是也有些不信任，为了达到更高的安全性，我们始终使用
** (虽然表现慢了一些)fsync()。如果你确定系统支持的fdatasync()始终
** 正确，那么就编译为-Dfdatasync=fdatasync.
**
*/
#if !defined(fdatasync)
# define fdatasync fsync
#endif

/*
** Define HAVE_FULLFSYNC to 0 or 1 depending on whether or not
** the F_FULLFSYNC macro is defined.  F_FULLFSYNC is currently
** only available on Mac OS X.  But that could change.
**
** 定义HAVE_FULLSYNC值为0或1取决于定义的F_FULLFSYNC宏。
** 但是F_FULLFSYNC宏仅在Mac OS X上可用，但有可能被改变。
**
*/
#ifdef F_FULLFSYNC
# define HAVE_FULLFSYNC 1
#else
# define HAVE_FULLFSYNC 0
#endif


/*
** The fsync() system call does not work as advertised on many
** unix systems.  The following procedure is an attempt to make
** it work better.
**
** The SQLITE_NO_SYNC macro disables all fsync()s.  This is useful
** for testing when we want to run through the test suite quickly.
** You are strongly advised *not* to deploy with SQLITE_NO_SYNC
** enabled, however, since with SQLITE_NO_SYNC enabled, an OS crash
** or power failure will likely corrupt the database file.
**
** fsync()系统调用在一些unix系统下并不像宣传的那样工作。以下的操作
** 旨在让它工作的更好
** SQLITE_NO_SYNC宏禁用所有的fsync()函数，当我们在测试时希望快速运行
** 通过测试套件时是有用的。强烈建议不要在部署的时候使用，但是
** 如果你在部署的时候启用了，一次操作系统出错或者是断电都会导致
** 数据库文件出错。
**
** SQLite sets the dataOnly flag if the size of the file is unchanged.
** The idea behind dataOnly is that it should only write the file content
** to disk, not the inode.  We only set dataOnly if the file size is 
** unchanged since the file size is part of the inode.  However, 
** Ted Ts'o tells us that fdatasync() will also write the inode if the
** file size has changed.  The only real difference between fdatasync()
** and fsync(), Ted tells us, is that fdatasync() will not flush the
** inode if the mtime or owner or other inode attributes have changed.
** We only care about the file size, not the other file attributes, so
** as far as SQLite is concerned, an fdatasync() is always adequate.
** So, we always use fdatasync() if it is available, regardless of
** the value of the dataOnly flag.
**
** SQLite在文件大小没有改变的情况下设置dataOnly标志。
** dataOnly背后的思想是只应该将文件内容写到磁盘，
** 而不是iNode中，由于文件大小是iNode的一部分，所以
** 我们仅设置dataOnly大小。然而，Ted Ts告诉我们fdatasync()
** 在文件大小改变的情况下写iNode。fdatasync()和fsync()之间
** 的实质区别在于mtime或拥有者或其他iNode属性已经被
** 改变，就SQLite而言，fdatasync()总是够用的，所以在fdatasync()
** 可用的情况下就直接使用，而不管dataOnly标志。
*/
static int full_fsync(int fd, int fullSync, int dataOnly){
  int rc;

  /* The following "ifdef/elif/else/" block has the same structure as
  ** the one below. It is replicated here solely to avoid cluttering 
  ** up the real code with the UNUSED_PARAMETER() macros.
  **
  ** 接下来的"ifdef/elif/else/"块具有相同的结构，
  ** 在这里的复制只是为了避免使用UNUSED_PARAMETER()
  ** 宏时出现混乱
  **
  */
#ifdef SQLITE_NO_SYNC
  UNUSED_PARAMETER(fd);
  UNUSED_PARAMETER(fullSync);
  UNUSED_PARAMETER(dataOnly);
#elif HAVE_FULLFSYNC
  UNUSED_PARAMETER(dataOnly);
#else
  UNUSED_PARAMETER(fullSync);
  UNUSED_PARAMETER(dataOnly);
#endif

  /* Record the number of times that we do a normal fsync() and 
  ** FULLSYNC.  This is used during testing to verify that this procedure
  ** gets called with the correct arguments.
  **
  ** 记录fsync() FULLSYNC正常执行的次数。这个在测试期间
  ** 验证正确参数调用情况。
  **
  */
#ifdef SQLITE_TEST
  if( fullSync ) sqlite3_fullsync_count++;
  sqlite3_sync_count++;
#endif

  /* If we compiled with the SQLITE_NO_SYNC flag, then syncing is a
  ** no-op
  */
#ifdef SQLITE_NO_SYNC
  rc = SQLITE_OK;
#elif HAVE_FULLFSYNC
  if( fullSync ){
    rc = osFcntl(fd, F_FULLFSYNC, 0);
  }else{
    rc = 1;
  }
  /* If the FULLFSYNC failed, fall back to attempting an fsync().
  ** It shouldn't be possible for fullfsync to fail on the local 
  ** file system (on OSX), so failure indicates that FULLFSYNC
  ** isn't supported for this file system. So, attempt an fsync 
  ** and (for now) ignore the overhead of a superfluous fcntl call.  
  ** It'd be better to detect fullfsync support once and avoid 
  ** the fcntl call every time sync is called.
  **
  ** 如果FULLFSYNC失败，则回退尝试使用fsync().在本地文件系统中
  ** fullsync不可能失败，所以FULLFSYNC失败表明它不适合这个文件系统
  ** 所以，对于fsync和一个多余的fcntl调用开销比较起来是可行的
  ** 这可以是一个检测fullfsync支持方法，从而避免每次调用sync时
  ** 对fcntl的调用。
  **
  */
  if( rc ) rc = fsync(fd);

#elif defined(__APPLE__)
  /* fdatasync() on HFS+ doesn't yet flush the file size if it changed correctly
  ** so currently we default to the macro that redefines fdatasync to fsync
  **
  ** fdatasync()在HFS+上没有文件大小刷新设置，如果正确的改变
  ** 所以我们默认的宏重新定义fdatasync和fsync
  */
  rc = fsync(fd);
#else 
  rc = fdatasync(fd);
#if OS_VXWORKS
  if( rc==-1 && errno==ENOTSUP ){
    rc = fsync(fd);
  }
#endif /* OS_VXWORKS */
#endif /* ifdef SQLITE_NO_SYNC elif HAVE_FULLFSYNC */

  if( OS_VXWORKS && rc!= -1 ){
    rc = 0;
  }
  return rc;
}

/*
** Open a file descriptor to the directory containing file zFilename.
** If successful, *pFd is set to the opened file descriptor and
** SQLITE_OK is returned. If an error occurs, either SQLITE_NOMEM
** or SQLITE_CANTOPEN is returned and *pFd is set to an undefined
** value.
**
** The directory file descriptor is used for only one thing - to
** fsync() a directory to make sure file creation and deletion events
** are flushed to disk.  Such fsyncs are not needed on newer
** journaling filesystems, but are required on older filesystems.
** 
** 如果成功，*pFd设置为打开的文件描述符并返回SQLITE_OK
** 如果出现错误，SQLITE_NOMEM或SQLITE_CANTOPEN都不会返回，
** *pFd设置为未定义值。
** 文件描述符只用于fsync()确定目录，从而确保文件创建和
** 删除事件都刷新到磁盘。这样的时间不需要更新到日志
** 文件系统中，但是在旧的文件系统需要备份、
**
** This routine can be overridden using the xSetSysCall interface.
** The ability to override this routine was added in support of the
** chromium sandbox.  Opening a directory is a security risk (we are
** told) so making it overrideable allows the chromium sandbox to
** replace this routine with a harmless no-op.  To make this routine
** a no-op, replace it with a stub that returns SQLITE_OK but leaves
** *pFd set to a negative number.
**
** If SQLITE_OK is returned, the caller is responsible for closing
** the file descriptor *pFd using close().
** 
** 本例程主要是使用xSetSysCall接口重写。主要是为了支持chromium
** 的沙盒。重写打开文件的安全风险所以可以在chromium的安全
** 沙盒中进行重写操作，为了实现返回值为SQLITE_OK将*pFd值设置
** 为负数。
** 如果SQLITE_OK是返回值，函数调用者需要使用*pFd关闭文件
*/
static int openDirectory(const char *zFilename, int *pFd){
  int ii;
  int fd = -1;
  char zDirname[MAX_PATHNAME+1];

  sqlite3_snprintf(MAX_PATHNAME, zDirname, "%s", zFilename);
  for(ii=(int)strlen(zDirname); ii>1 && zDirname[ii]!='/'; ii--);
  if( ii>0 ){
    zDirname[ii] = '\0';
    fd = robust_open(zDirname, O_RDONLY|O_BINARY, 0);
    if( fd>=0 ){
      OSTRACE(("OPENDIR %-3d %s\n", fd, zDirname));
    }
  }
  *pFd = fd;
  return (fd>=0?SQLITE_OK:unixLogError(SQLITE_CANTOPEN_BKPT, "open", zDirname));
}

/*
** Make sure all writes to a particular file are committed to disk.
**
** If dataOnly==0 then both the file itself and its metadata (file
** size, access time, etc) are synced.  If dataOnly!=0 then only the
** file data is synced.
**
** Under Unix, also make sure that the directory entry for the file
** has been created by fsync-ing the directory that contains the file.
** If we do not do this and we encounter a power failure, the directory
** entry for the journal might not exist after we reboot.  The next
** SQLite to access the file will not know that the journal exists (because
** the directory entry for the journal was never created) and the transaction
** will not roll back - possibly leading to database corruption.
*/
static int unixSync(sqlite3_file *id, int flags){
  int rc;
  unixFile *pFile = (unixFile*)id;

  int isDataOnly = (flags&SQLITE_SYNC_DATAONLY);
  int isFullsync = (flags&0x0F)==SQLITE_SYNC_FULL;

  /* Check that one of SQLITE_SYNC_NORMAL or FULL was passed */
  assert((flags&0x0F)==SQLITE_SYNC_NORMAL
      || (flags&0x0F)==SQLITE_SYNC_FULL
  );

  /* Unix cannot, but some systems may return SQLITE_FULL from here. This
  ** line is to test that doing so does not cause any problems.
  */
  SimulateDiskfullError( return SQLITE_FULL );

  assert( pFile );
  OSTRACE(("SYNC    %-3d\n", pFile->h));
  rc = full_fsync(pFile->h, isFullsync, isDataOnly);
  SimulateIOError( rc=1 );
  if( rc ){
    pFile->lastErrno = errno;
    return unixLogError(SQLITE_IOERR_FSYNC, "full_fsync", pFile->zPath);
  }

  /* Also fsync the directory containing the file if the DIRSYNC flag
  ** is set.  This is a one-time occurrance.  Many systems (examples: AIX)
  ** are unable to fsync a directory, so ignore errors on the fsync.
  */
  if( pFile->ctrlFlags & UNIXFILE_DIRSYNC ){
    int dirfd;
    OSTRACE(("DIRSYNC %s (have_fullfsync=%d fullsync=%d)\n", pFile->zPath,
            HAVE_FULLFSYNC, isFullsync));
    rc = osOpenDirectory(pFile->zPath, &dirfd);
    if( rc==SQLITE_OK && dirfd>=0 ){
      full_fsync(dirfd, 0, 0);
      robust_close(pFile, dirfd, __LINE__);
    }else if( rc==SQLITE_CANTOPEN ){
      rc = SQLITE_OK;
    }
    pFile->ctrlFlags &= ~UNIXFILE_DIRSYNC;
  }
  return rc;
}

/*
** Truncate an open file to a specified size
**
** 将文件截断为指定大小
*/
static int unixTruncate(sqlite3_file *id, i64 nByte){
  unixFile *pFile = (unixFile *)id;
  int rc;
  assert( pFile );
  SimulateIOError( return SQLITE_IOERR_TRUNCATE );

  /* If the user has configured a chunk-size for this file, truncate the
  ** file so that it consists of an integer number of chunks (i.e. the
  ** actual file size after the operation may be larger than the requested
  ** size).
  ** 
  ** 如果用户为本文件设置了块大小，那么就截断文件
  ** 从而由整数块组成(实际文件在操作后可能大于请求的
  ** 尺寸大小)
  */
  if( pFile->szChunk>0 ){
    nByte = ((nByte + pFile->szChunk - 1)/pFile->szChunk) * pFile->szChunk;
  }

  rc = robust_ftruncate(pFile->h, (off_t)nByte);
  if( rc ){
    pFile->lastErrno = errno;
    return unixLogError(SQLITE_IOERR_TRUNCATE, "ftruncate", pFile->zPath);
  }else{
#ifdef SQLITE_DEBUG
    /* If we are doing a normal write to a database file (as opposed to
    ** doing a hot-journal rollback or a write to some file other than a
    ** normal database file) and we truncate the file to zero length,
    ** that effectively updates the change counter.  This might happen
    ** when restoring a database using the backup API from a zero-length
    ** source.
    **
    ** 如果我们对数据库文件进行正常写入(而不是执行
    ** 热回滚或写入其他文件而不是一个正常的数据库文件)
    ** 我们将文件长度截断为0，这个可能在使用从长度为0的
    ** 数据库文件中恢复数据。
    */
    if( pFile->inNormalWrite && nByte==0 ){
      pFile->transCntrChng = 1;
    }
#endif

    return SQLITE_OK;
  }
}

/*
** Determine the current size of a file in bytes
** 按字节统计当前文件的大小
*/
static int unixFileSize(sqlite3_file *id, i64 *pSize){
  int rc;
  struct stat buf;
  assert( id );
  rc = osFstat(((unixFile*)id)->h, &buf);
  SimulateIOError( rc=1 );
  if( rc!=0 ){
    ((unixFile*)id)->lastErrno = errno;
    return SQLITE_IOERR_FSTAT;
  }
  *pSize = buf.st_size;

  /* When opening a zero-size database, the findInodeInfo() procedure
  ** writes a single byte into that file in order to work around a bug
  ** in the OS-X msdos filesystem.  In order to avoid problems with upper
  ** layers, we need to report this file size as zero even though it is
  ** really 1.   Ticket #3260.
  ** 
  ** 当打开数据库的大小为0时，findInodeInfo()过程中写入一个字节到该文件
  ** 从而在OS-Xmsdos文件系统中解决这个问题。为了避免上层系统出现问题
  ** 我们需要报告这个文件为0，即使也为1，详见#3260
  */
  if( *pSize==1 ) *pSize = 0;


  return SQLITE_OK;
}

#if SQLITE_ENABLE_LOCKING_STYLE && defined(__APPLE__)
/*
** Handler for proxy-locking file-control verbs.  Defined below in the
** proxying locking division.
*/
static int proxyFileControl(sqlite3_file*,int,void*);
#endif

/* 
** This function is called to handle the SQLITE_FCNTL_SIZE_HINT 
** file-control operation.  Enlarge the database to nBytes in size
** (rounded up to the next chunk-size).  If the database is already
** nBytes or larger, this routine is a no-op.
** 
** 这个函数主要是处理SQLITE_FCNTL_SIZE_HINT文件控制操作，将数据库
** 大小放大到nBytes(向下舍入为下一个块大小)。如果数据库已经是
** nBytes或更大，本操作无效。
*/
static int fcntlSizeHint(unixFile *pFile, i64 nByte){
  if( pFile->szChunk>0 ){
    i64 nSize;                    /* Required file size */
    struct stat buf;              /* Used to hold return values of fstat() */
   
    if( osFstat(pFile->h, &buf) ) return SQLITE_IOERR_FSTAT;

    nSize = ((nByte+pFile->szChunk-1) / pFile->szChunk) * pFile->szChunk;
    if( nSize>(i64)buf.st_size ){

#if defined(HAVE_POSIX_FALLOCATE) && HAVE_POSIX_FALLOCATE
      /* The code below is handling the return value of osFallocate() 
      ** correctly. posix_fallocate() is defined to "returns zero on success, 
      ** or an error number on  failure". See the manpage for details. 
      ** 下面的代码是正确处理osFallocate() 返回值，posix_fallocate()定义为"
      ** 返回值为0成功，出错时为对于编号"详细信息见帮助页*/
      int err;
      do{
        err = osFallocate(pFile->h, buf.st_size, nSize-buf.st_size);
      }while( err==EINTR );
      if( err ) return SQLITE_IOERR_WRITE;
#else
      /* If the OS does not have posix_fallocate(), fake it. First use
      ** ftruncate() to set the file size, then write a single byte to
      ** the last byte in each block within the extended region. This
      ** is the same technique used by glibc to implement posix_fallocate()
      ** on systems that do not have a real fallocate() system call.
      **
      ** 如果首先假定操作系统没有posix_fallocate()，首先使用
      ** dtruncate()设置文件块大小。然后在每个块的扩展区域写入
      ** 一个字节，这个和glibc实现posix_fallocate()的原理一致。在
      ** 没有实现fallocate()系统调用的系统上。
      */
      int nBlk = buf.st_blksize;  /* File-system block size */
      i64 iWrite;                 /* Next offset to write to */

      if( robust_ftruncate(pFile->h, nSize) ){
        pFile->lastErrno = errno;
        return unixLogError(SQLITE_IOERR_TRUNCATE, "ftruncate", pFile->zPath);
      }
      iWrite = ((buf.st_size + 2*nBlk - 1)/nBlk)*nBlk-1;
      while( iWrite<nSize ){
        int nWrite = seekAndWrite(pFile, iWrite, "", 1);
        if( nWrite!=1 ) return SQLITE_IOERR_WRITE;
        iWrite += nBlk;
      }
#endif
    }
  }

  return SQLITE_OK;
}

/*
** If *pArg is inititially negative then this is a query.  Set *pArg to
** 1 or 0 depending on whether or not bit mask of pFile->ctrlFlags is set.
**
** If *pArg is 0 or 1, then clear or set the mask bit of pFile->ctrlFlags.
** 如果*pArg初始值为负，那么就是一个查询语句，设置为1或0取决于
** 是否已经设置了pFile->ctrlFlags的位掩码。
** 
** 如果*pArg为0或1，则清除或者设置pFile->ctrlFlsgd的位掩码。
*/
static void unixModeBit(unixFile *pFile, unsigned char mask, int *pArg){
  if( *pArg<0 ){
    *pArg = (pFile->ctrlFlags & mask)!=0;
  }else if( (*pArg)==0 ){
    pFile->ctrlFlags &= ~mask;
  }else{
    pFile->ctrlFlags |= mask;
  }
}

/*
** Information and control of an open file handle.
** 文件控制句柄和对应的信息。
*/
static int unixFileControl(sqlite3_file *id, int op, void *pArg){
  unixFile *pFile = (unixFile*)id;
  switch( op ){
    case SQLITE_FCNTL_LOCKSTATE: {
      *(int*)pArg = pFile->eFileLock;
      return SQLITE_OK;
    }
    case SQLITE_LAST_ERRNO: {
      *(int*)pArg = pFile->lastErrno;
      return SQLITE_OK;
    }
    case SQLITE_FCNTL_CHUNK_SIZE: {
      pFile->szChunk = *(int *)pArg;
      return SQLITE_OK;
    }
    case SQLITE_FCNTL_SIZE_HINT: {
      int rc;
      SimulateIOErrorBenign(1);
      rc = fcntlSizeHint(pFile, *(i64 *)pArg);
      SimulateIOErrorBenign(0);
      return rc;
    }
    case SQLITE_FCNTL_PERSIST_WAL: {
      unixModeBit(pFile, UNIXFILE_PERSIST_WAL, (int*)pArg);
      return SQLITE_OK;
    }
    case SQLITE_FCNTL_POWERSAFE_OVERWRITE: {
      unixModeBit(pFile, UNIXFILE_PSOW, (int*)pArg);
      return SQLITE_OK;
    }
    case SQLITE_FCNTL_VFSNAME: {
      *(char**)pArg = sqlite3_mprintf("%s", pFile->pVfs->zName);
      return SQLITE_OK;
    }
#ifdef SQLITE_DEBUG
    /* The pager calls this method to signal that it has done
    ** a rollback and that the database is therefore unchanged and
    ** it hence it is OK for the transaction change counter to be
    ** unchanged.
    **
    ** 页面调用这个方法表示已经完成了回滚操作，数据库也不会
    ** 进行改变，而这也表明事务改变计数器可以不用改变
    */
    case SQLITE_FCNTL_DB_UNCHANGED: {
      ((unixFile*)id)->dbUpdate = 0;
      return SQLITE_OK;
    }
#endif
#if SQLITE_ENABLE_LOCKING_STYLE && defined(__APPLE__)
    case SQLITE_SET_LOCKPROXYFILE:
    case SQLITE_GET_LOCKPROXYFILE: {
      return proxyFileControl(id,op,pArg);
    }
#endif /* SQLITE_ENABLE_LOCKING_STYLE && defined(__APPLE__) */
  }
  return SQLITE_NOTFOUND;
}

/*
** Return the sector size in bytes of the underlying block device for
** the specified file. This is almost always 512 bytes, but may be
** larger for some devices.
**
** SQLite code assumes this function cannot fail. It also assumes that
** if two files are created in the same file-system directory (i.e.
** a database and its journal file) that the sector size will be the
** same for both.
**
** 返回指定文件基本数据块的字节大小。一般情况下为512字节
** 但在一些设备上可能更大。
** 
** SQLite代码假定这个函数不会出错，同时假定在同一个系统文件
** 目录下创建两个文件(数据库和日志文件)，两者的扇区大小相同。
*/
static int unixSectorSize(sqlite3_file *pFile){
  (void)pFile;
  return SQLITE_DEFAULT_SECTOR_SIZE;
}

/*
** Return the device characteristics for the file.
**
** This VFS is set up to return SQLITE_IOCAP_POWERSAFE_OVERWRITE by default.
** However, that choice is contraversial since technically the underlying
** file system does not always provide powersafe overwrites.  (In other
** words, after a power-loss event, parts of the file that were never
** written might end up being altered.)  However, non-PSOW behavior is very,
** very rare.  And asserting PSOW makes a large reduction in the amount
** of required I/O for journaling, since a lot of padding is eliminated.
**  Hence, while POWERSAFE_OVERWRITE is on by default, there is a file-control
** available to turn it off and URI query parameter available to turn it off.
** 
** 返回设备文件的特点
** VFS默认情况下设置为返回SQLITE_IOCAP_POWERSAFE_OVERWRITE，
** 然而，这种选择是可以逆转的，因为系统不一定提供了
** 电源安全文件机制。(在断电后，部分被改变的文件不一定
** 已经实现了改变).然而，无PSOW行为极其稀少。同时，声明
** PSOW行为大大减少了日志需要的IO操作。这其中消除了很多
** 填充操作。所以，默认情况下POWERSAFE_OVERWRITE是打开的，
** 也有一个可选的文件控制选项去关闭，URI查询参数也可以
** 关闭它。
*/
static int unixDeviceCharacteristics(sqlite3_file *id){
  unixFile *p = (unixFile*)id;
  if( p->ctrlFlags & UNIXFILE_PSOW ){
    return SQLITE_IOCAP_POWERSAFE_OVERWRITE;
  }else{
    return 0;
  }
}

#ifndef SQLITE_OMIT_WAL


/*
** Object used to represent an shared memory buffer.  
**
** When multiple threads all reference the same wal-index, each thread
** has its own unixShm object, but they all point to a single instance
** of this unixShmNode object.  In other words, each wal-index is opened
** only once per process.
**
** Each unixShmNode object is connected to a single unixInodeInfo object.
** We could coalesce this object into unixInodeInfo, but that would mean
** every open file that does not use shared memory (in other words, most
** open files) would have to carry around this extra information.  So
** the unixInodeInfo object contains a pointer to this unixShmNode object
** and the unixShmNode object is created only when needed.
**
** 用于表示共享内存缓冲区对象
** 当多线程都引用同一个wal-index，每个线程都有自己的unixShm对象
** 但是都指向单个实例，这个unixShmNode对象都指向同一个实例。
** 换句话说，每个wal-index 只能被一个进程打开。
**
** 每个unixShmNode对象连接到单个unixInodeInfo对象。
** 我们可以将这个对象合并到unixInodeInfo，但这是意味着
** 每个打开的文件，不使用共享内存（换句话说，大多数
** 打开文件）将不得不携带这些额外的信息。所以
** unixInodeInfo对象包含指向此unixShmNode对象的指针
** 和unixShmNode对象仅在需要时创建。
**
** unixMutexHeld() must be true when creating or destroying
** this object or while reading or writing the following fields:
**
**      nRef
**
** The following fields are read-only after the object is created:
** 
**      fid
**      zFilename
**
** Either unixShmNode.mutex must be held or unixShmNode.nRef==0 and
** unixMutexHeld() is true when reading or writing any other field
** in this structure.
**
** unixMutexHeld（）在创建或销毁时必须为true
** 此对象或在读取或写入以下字段时：
**
**  nRef
**
** 在创建对象后，以下字段为只读：
**
**  fid
**  zFilename
**
** unixShmNode.mutex必须保持或unixShmNode.nRef == 0和
** unixMutexHeld（）在读取或写入任何其他字段时为true
** 在此结构中。
**
*/
struct unixShmNode {
  unixInodeInfo *pInode;     /* unixInodeInfo that owns this SHM node */
  sqlite3_mutex *mutex;      /* Mutex to access this object */
  char *zFilename;           /* Name of the mmapped file */
  int h;                     /* Open file descriptor */
  int szRegion;              /* Size of shared-memory regions */
  u16 nRegion;               /* Size of array apRegion */
  u8 isReadonly;             /* True if read-only */
  char **apRegion;           /* Array of mapped shared-memory regions */
  int nRef;                  /* Number of unixShm objects pointing to this */
  unixShm *pFirst;           /* All unixShm objects pointing to this */
#ifdef SQLITE_DEBUG
  u8 exclMask;               /* Mask of exclusive locks held */
  u8 sharedMask;             /* Mask of shared locks held */
  u8 nextShmId;              /* Next available unixShm.id value */
#endif
};

/*
** Structure used internally by this VFS to record the state of an
** open shared memory connection.
**
** The following fields are initialized when this object is created and
** are read-only thereafter:
**
**    unixShm.pFile
**    unixShm.id
**
** All other fields are read/write.  The unixShm.pFile->mutex must be held
** while accessing any read/write fields.
**
** 此VFS内部使用的结构，用于记录打开共享内存连接的状态
** 
**
** 创建此对象初始化之后这些字段只读：
**
**  unixShm.pFile
**  unixShm.id
**
** 所有其他字段为读/写。在访问任何读/写字段必须保持unixShm.pFile-> mutex
** 
*/
struct unixShm {
  unixShmNode *pShmNode;     /* The underlying unixShmNode object */
  unixShm *pNext;            /* Next unixShm with the same unixShmNode */
  u8 hasMutex;               /* True if holding the unixShmNode mutex */
  u8 id;                     /* Id of this connection within its unixShmNode */
  u16 sharedMask;            /* Mask of shared locks held */
  u16 exclMask;              /* Mask of exclusive locks held */
};

/*
** Constants used for locking 用于锁定的常数
*/
#define UNIX_SHM_BASE   ((22+SQLITE_SHM_NLOCK)*4)         /* first lock byte */
#define UNIX_SHM_DMS    (UNIX_SHM_BASE+SQLITE_SHM_NLOCK)  /* deadman switch */

/*
** Apply posix advisory locks for all bytes from ofst through ofst+n-1.
**
** Locks block if the mask is exactly UNIX_SHM_C and are non-blocking
** otherwise.
**
** 对于从第1到第n + 1的所有字节应用posix咨询锁。
**
** 如果掩码正好是UNIX_SHM_C，则锁定块，否则为非阻塞。
**
*/
static int unixShmSystemLock(
  unixShmNode *pShmNode, /* Apply locks to this open shared-memory segment */
  int lockType,          /* F_UNLCK, F_RDLCK, or F_WRLCK */
  int ofst,              /* First byte of the locking range */
  int n                  /* Number of bytes to lock */
){
  struct flock f;       /* The posix advisory locking structure */
  int rc = SQLITE_OK;   /* Result code form fcntl() */

  /* Access to the unixShmNode object is serialized by the caller */
  assert( sqlite3_mutex_held(pShmNode->mutex) || pShmNode->nRef==0 );

  /* Shared locks never span more than one byte */
  assert( n==1 || lockType!=F_RDLCK );

  /* Locks are within range */
  assert( n>=1 && n<SQLITE_SHM_NLOCK );

  if( pShmNode->h>=0 ){
    /* Initialize the locking parameters */
    memset(&f, 0, sizeof(f));
    f.l_type = lockType;
    f.l_whence = SEEK_SET;
    f.l_start = ofst;
    f.l_len = n;

    rc = osFcntl(pShmNode->h, F_SETLK, &f);
    rc = (rc!=(-1)) ? SQLITE_OK : SQLITE_BUSY;
  }

  /* Update the global lock state and do debug tracing 
  ** 更新全局锁定状态并执行调试跟踪
  */
#ifdef SQLITE_DEBUG
  { u16 mask;
  OSTRACE(("SHM-LOCK "));
  mask = (1<<(ofst+n)) - (1<<ofst);
  if( rc==SQLITE_OK ){
    if( lockType==F_UNLCK ){
      OSTRACE(("unlock %d ok", ofst));
      pShmNode->exclMask &= ~mask;
      pShmNode->sharedMask &= ~mask;
    }else if( lockType==F_RDLCK ){
      OSTRACE(("read-lock %d ok", ofst));
      pShmNode->exclMask &= ~mask;
      pShmNode->sharedMask |= mask;
    }else{
      assert( lockType==F_WRLCK );
      OSTRACE(("write-lock %d ok", ofst));
      pShmNode->exclMask |= mask;
      pShmNode->sharedMask &= ~mask;
    }
  }else{
    if( lockType==F_UNLCK ){
      OSTRACE(("unlock %d failed", ofst));
    }else if( lockType==F_RDLCK ){
      OSTRACE(("read-lock failed"));
    }else{
      assert( lockType==F_WRLCK );
      OSTRACE(("write-lock %d failed", ofst));
    }
  }
  OSTRACE((" - afterwards %03x,%03x\n",
           pShmNode->sharedMask, pShmNode->exclMask));
  }
#endif

  return rc;        
}


/*
** Purge the unixShmNodeList list of all entries with unixShmNode.nRef==0.
**
** This is not a VFS shared-memory method; it is a utility function called
** by VFS shared-memory methods.
**
** 使用unixShmNode.nRef == 0清除所有条目的unixShmNodeList列表。
** 这不是一个VFS共享内存方法; 它是一个由VFS共享内存方法调用的效用函数。
**
*/
static void unixShmPurge(unixFile *pFd){
  unixShmNode *p = pFd->pInode->pShmNode;
  assert( unixMutexHeld() );
  if( p && p->nRef==0 ){
    int i;
    assert( p->pInode==pFd->pInode );
    sqlite3_mutex_free(p->mutex);
    for(i=0; i<p->nRegion; i++){
      if( p->h>=0 ){
        munmap(p->apRegion[i], p->szRegion);
      }else{
        sqlite3_free(p->apRegion[i]);
      }
    }
    sqlite3_free(p->apRegion);
    if( p->h>=0 ){
      robust_close(pFd, p->h, __LINE__);
      p->h = -1;
    }
    p->pInode->pShmNode = 0;
    sqlite3_free(p);
  }
}

/*
** Open a shared-memory area associated with open database file pDbFd.  
** This particular implementation uses mmapped files.
**
** The file used to implement shared-memory is in the same directory
** as the open database file and has the same name as the open database
** file with the "-shm" suffix added.  For example, if the database file
** is "/home/user1/config.db" then the file that is created and mmapped
** for shared memory will be called "/home/user1/config.db-shm".  
**
** Another approach to is to use files in /dev/shm or /dev/tmp or an
** some other tmpfs mount. But if a file in a different directory
** from the database file is used, then differing access permissions
** or a chroot() might cause two different processes on the same
** database to end up using different files for shared memory - 
** meaning that their memory would not really be shared - resulting
** in database corruption.  Nevertheless, this tmpfs file usage
** can be enabled at compile-time using -DSQLITE_SHM_DIRECTORY="/dev/shm"
** or the equivalent.  The use of the SQLITE_SHM_DIRECTORY compile-time
** option results in an incompatible build of SQLite;  builds of SQLite
** that with differing SQLITE_SHM_DIRECTORY settings attempt to use the
** same database file at the same time, database corruption will likely
** result. The SQLITE_SHM_DIRECTORY compile-time option is considered
** "unsupported" and may go away in a future SQLite release.
**
** 打开与打开的数据库文件pDbFd相关联的共享内存区域。
** 此特定实现使用mmapped文件。
**
** 用于实现共享内存的文件与打开的数据库文件在同一目录中，
** 并且与添加了“-shm”后缀的打开数据库文件具有相同的名称。
** 例如，如果数据库文件是“/home/user1/config.db”，则为共享内存创建
** 和mmapped的文件将被称为“/home/user1/config.db-shm”。
**
** 另一种方法是使用/ dev / shm或/ dev /tmp中的文件或安装其他一些tmpfs。
** 但是，如果使用与数据库文件不同目录中的文件，则不同的访问
** 权限或chroot（）可能导致同一数据库上的两个不同的进程同时使用
** 不同的文件共享内存，但是实际上他们的内存不会真的共享,这将
** 导致数据库损坏。但是，我们可以在编译时使用
** -DSQLITE_SHM_DIRECTORY=“/ dev /shm”或等效命令来启用此tmpfs文件使用。
** 使用SQLITE_SHM_DIRECTORY编译时选项会导致SQLite的不兼容构建;
** SQLite的构建使用不同的SQLITE_SHM_DIRECTORY设置尝试在同一时间使用
** 相同的数据库文件，数据库损坏可能会导致。编译时选项
** SQLITE_SHM_DIRECTORY被认为是“不支持”，这个特性可能在未来SQLite中消失。
**
** When opening a new shared-memory file, if no other instances of that
** file are currently open, in this process or in other processes, then
** the file must be truncated to zero length or have its header cleared.
**
** If the original database file (pDbFd) is using the "unix-excl" VFS
** that means that an exclusive lock is held on the database file and
** that no other processes are able to read or write the database.  In
** that case, we do not really need shared memory.  No shared memory
** file is created.  The shared memory will be simulated with heap memory.
**
** 打开新的共享内存文件时，如果当前文件没有的其他打开实例，
** 则在此过程或其他进程中，该文件必须截断为零长度或清除其头部。
** 如果原始数据库文件（pDbFd）使用“unix-excl”VFS，这意味着对数据库
** 文件保留独占锁，并且没有其他进程能够读取或写入数据库。 
** 在这种情况下，我们不需要共享内存。 不会创建共享内存文件。 
** 使用堆内存进行模拟共享内存。
**
*/
static int unixOpenSharedMemory(unixFile *pDbFd){
  struct unixShm *p = 0;          /* The connection to be opened */
  struct unixShmNode *pShmNode;   /* The underlying mmapped file */
  int rc;                         /* Result code */
  unixInodeInfo *pInode;          /* The inode of fd */
  char *zShmFilename;             /* Name of the file used for SHM */
  int nShmFilename;               /* Size of the SHM filename in bytes */

  /* Allocate space for the new unixShm object. */
  p = sqlite3_malloc( sizeof(*p) );
  if( p==0 ) return SQLITE_NOMEM;
  memset(p, 0, sizeof(*p));
  assert( pDbFd->pShm==0 );

  /* Check to see if a unixShmNode object already exists. Reuse an existing
  ** one if present. Create a new one if necessary.
  **
  ** 检查unixShmNode对象是否已存在。 存在就重复使用现有的对象。
  ** 必要时，创建一个新的对象。
  **
  */
  unixEnterMutex();
  pInode = pDbFd->pInode;
  pShmNode = pInode->pShmNode;
  if( pShmNode==0 ){
    struct stat sStat;                 /* fstat() info for database file */

    /* Call fstat() to figure out the permissions on the database file. If
    ** a new *-shm file is created, an attempt will be made to create it
    ** with the same permissions.
    **
    ** 调用fstat（）来查找数据库文件的权限。 如果创建了新的* -shm文件，
    ** 将尝试使用相同的权限创建它。
    */
    if( osFstat(pDbFd->h, &sStat) && pInode->bProcessLock==0 ){
      rc = SQLITE_IOERR_FSTAT;
      goto shm_open_err;
    }

#ifdef SQLITE_SHM_DIRECTORY
    nShmFilename = sizeof(SQLITE_SHM_DIRECTORY) + 31;
#else
    nShmFilename = 6 + (int)strlen(pDbFd->zPath);
#endif
    pShmNode = sqlite3_malloc( sizeof(*pShmNode) + nShmFilename );
    if( pShmNode==0 ){
      rc = SQLITE_NOMEM;
      goto shm_open_err;
    }
    memset(pShmNode, 0, sizeof(*pShmNode)+nShmFilename);
    zShmFilename = pShmNode->zFilename = (char*)&pShmNode[1];
#ifdef SQLITE_SHM_DIRECTORY
    sqlite3_snprintf(nShmFilename, zShmFilename, 
                     SQLITE_SHM_DIRECTORY "/sqlite-shm-%x-%x",
                     (u32)sStat.st_ino, (u32)sStat.st_dev);
#else
    sqlite3_snprintf(nShmFilename, zShmFilename, "%s-shm", pDbFd->zPath);
    sqlite3FileSuffix3(pDbFd->zPath, zShmFilename);
#endif
    pShmNode->h = -1;
    pDbFd->pInode->pShmNode = pShmNode;
    pShmNode->pInode = pDbFd->pInode;
    pShmNode->mutex = sqlite3_mutex_alloc(SQLITE_MUTEX_FAST);
    if( pShmNode->mutex==0 ){
      rc = SQLITE_NOMEM;
      goto shm_open_err;
    }

    if( pInode->bProcessLock==0 ){
      int openFlags = O_RDWR | O_CREAT;
      if( sqlite3_uri_boolean(pDbFd->zPath, "readonly_shm", 0) ){
        openFlags = O_RDONLY;
        pShmNode->isReadonly = 1;
      }
      pShmNode->h = robust_open(zShmFilename, openFlags, (sStat.st_mode&0777));
      if( pShmNode->h<0 ){
        rc = unixLogError(SQLITE_CANTOPEN_BKPT, "open", zShmFilename);
        goto shm_open_err;
      }

      /* If this process is running as root, make sure that the SHM file
      ** is owned by the same user that owns the original database.  Otherwise,
      ** the original owner will not be able to connect.
      **
      ** 如果此进程以root身份运行，请确保SHM文件由原始数据库的同一用户拥有。
      ** 否则，原始所有者也将无法连接。
      **
      */
      osFchown(pShmNode->h, sStat.st_uid, sStat.st_gid);
  
      /* Check to see if another process is holding the dead-man switch.
      ** If not, truncate the file to zero length. 
      **
      ** 检查另一个进程是否持有dead-man开关。如果没有，请将文件截断为零长度。
      */
      rc = SQLITE_OK;
      if( unixShmSystemLock(pShmNode, F_WRLCK, UNIX_SHM_DMS, 1)==SQLITE_OK ){
        if( robust_ftruncate(pShmNode->h, 0) ){
          rc = unixLogError(SQLITE_IOERR_SHMOPEN, "ftruncate", zShmFilename);
        }
      }
      if( rc==SQLITE_OK ){
        rc = unixShmSystemLock(pShmNode, F_RDLCK, UNIX_SHM_DMS, 1);
      }
      if( rc ) goto shm_open_err;
    }
  }

  /* Make the new connection a child of the unixShmNode */
  p->pShmNode = pShmNode;
#ifdef SQLITE_DEBUG
  p->id = pShmNode->nextShmId++;
#endif
  pShmNode->nRef++;
  pDbFd->pShm = p;
  unixLeaveMutex();

  /* The reference count on pShmNode has already been incremented under
  ** the cover of the unixEnterMutex() mutex and the pointer from the
  ** new (struct unixShm) object to the pShmNode has been set. All that is
  ** left to do is to link the new object into the linked list starting
  ** at pShmNode->pFirst. This must be done while holding the pShmNode->mutex 
  ** mutex.
  ** 
  ** pShmNode上的引用计数已在unixEnterMutex（）互斥条件下增加，
  ** 并且已设置从新创建的（struct unixShm）对象到pShmNode的指针。 
  ** 所有剩下要做的是将该对象链接到以pShmNode->pFirst开始的列表中。 
  ** 这些操作都必须同时保持pShmNode-> mutex.
  */
  sqlite3_mutex_enter(pShmNode->mutex);
  p->pNext = pShmNode->pFirst;
  pShmNode->pFirst = p;
  sqlite3_mutex_leave(pShmNode->mutex);
  return SQLITE_OK;

  /* Jump here on any error  发生错误时调到此处*/
shm_open_err:
  unixShmPurge(pDbFd);       /* This call frees pShmNode if required */
  sqlite3_free(p);
  unixLeaveMutex();
  return rc;
}

/*
** This function is called to obtain a pointer to region iRegion of the 
** shared-memory associated with the database file fd. Shared-memory regions 
** are numbered starting from zero. Each shared-memory region is szRegion 
** bytes in size.
**
** If an error occurs, an error code is returned and *pp is set to NULL.
**
** Otherwise, if the bExtend parameter is 0 and the requested shared-memory
** region has not been allocated (by any client, including one running in a
** separate process), then *pp is set to NULL and SQLITE_OK returned. If 
** bExtend is non-zero and the requested shared-memory region has not yet 
** been allocated, it is allocated by this function.
**
** If the shared-memory region has already been allocated or is allocated by
** this call as described above, then it is mapped into this processes 
** address space (if it is not already), *pp is set to point to the mapped 
** memory and SQLITE_OK returned.
**
** 调用该函数以获得指向与数据库文件fd相关联的共享存储器的区域iRegion的指针。
** 共享内存区域从零开始编号。 每个共享内存区域的字节大小为szRegion。
**
** 如果发生错误，则返回错误代码，并将* pp设置为NULL。
**
** 否则，如果bExtend参数为0，并且尚未分配请求的共享内存区域（对于任何客户端，
** 包括在独立进程中运行的客户端），则* pp将设置为NULL，并返回SQLITE_OK。 如果
** bExtend不为零，并且所请求的共享内存区域尚未分配，则由此函数分配。
**
** 如果共享内存区域已经被分配或通过上述的调用分配，则它被映射到这个进程
** 地址空间（如果它还没有确定），则设置*pp为指向映射的存储器，返回值为SQLITE_OK。
**
*/
static int unixShmMap(
  sqlite3_file *fd,               /* Handle open on database file */
  int iRegion,                    /* Region to retrieve */
  int szRegion,                   /* Size of regions */
  int bExtend,                    /* True to extend file if necessary */
  void volatile **pp              /* OUT: Mapped memory */
){
  unixFile *pDbFd = (unixFile*)fd;
  unixShm *p;
  unixShmNode *pShmNode;
  int rc = SQLITE_OK;

  /* If the shared-memory file has not yet been opened, open it now. 
  ** 如果共享内存文件尚未打开，请立即打开它。
  */
  if( pDbFd->pShm==0 ){
    rc = unixOpenSharedMemory(pDbFd);
    if( rc!=SQLITE_OK ) return rc;
  }

  p = pDbFd->pShm;
  pShmNode = p->pShmNode;
  sqlite3_mutex_enter(pShmNode->mutex);
  assert( szRegion==pShmNode->szRegion || pShmNode->nRegion==0 );
  assert( pShmNode->pInode==pDbFd->pInode );
  assert( pShmNode->h>=0 || pDbFd->pInode->bProcessLock==1 );
  assert( pShmNode->h<0 || pDbFd->pInode->bProcessLock==0 );

  if( pShmNode->nRegion<=iRegion ){
    char **apNew;                      /* New apRegion[] array */
    int nByte = (iRegion+1)*szRegion;  /* Minimum required file size */
    struct stat sStat;                 /* Used by fstat() */

    pShmNode->szRegion = szRegion;

    if( pShmNode->h>=0 ){
      /* The requested region is not mapped into this processes address space.
      ** Check to see if it has been allocated (i.e. if the wal-index file is
      ** large enough to contain the requested region).
      **
      ** 请求的区域不会映射到此进程地址空间。检查
      ** 它是否已经被分配（即：如果在wal索引文件大到包含所请求的区域）。
      **
      */
      if( osFstat(pShmNode->h, &sStat) ){
        rc = SQLITE_IOERR_SHMSIZE;
        goto shmpage_out;
      }
  
      if( sStat.st_size<nByte ){
        /* The requested memory region does not exist. If bExtend is set to
        ** false, exit early. *pp will be set to NULL and SQLITE_OK returned.
        **
        ** Alternatively, if bExtend is true, use ftruncate() to allocate
        ** the requested memory region.
        **
        ** 请求的内存区域不存在。 如果bExtend设置为false，则提前退出。 
        ** 并将*pp设置为NULL且返回SQLITE_OK。
        **
        ** 或者，如果bExtend为true，则使用ftruncate（）分配所请求的内存区域。
        **
        */
        if( !bExtend ) goto shmpage_out;
        if( robust_ftruncate(pShmNode->h, nByte) ){
          rc = unixLogError(SQLITE_IOERR_SHMSIZE, "ftruncate",
                            pShmNode->zFilename);
          goto shmpage_out;
        }
      }
    }

    /* Map the requested memory region into this processes address space. 
	** 将请求的内存区域映射到此进程地址空间。
	*/
    apNew = (char **)sqlite3_realloc(
        pShmNode->apRegion, (iRegion+1)*sizeof(char *)
    );
    if( !apNew ){
      rc = SQLITE_IOERR_NOMEM;
      goto shmpage_out;
    }
    pShmNode->apRegion = apNew;
    while(pShmNode->nRegion<=iRegion){
      void *pMem;
      if( pShmNode->h>=0 ){
        pMem = mmap(0, szRegion,
            pShmNode->isReadonly ? PROT_READ : PROT_READ|PROT_WRITE, 
            MAP_SHARED, pShmNode->h, pShmNode->nRegion*szRegion
        );
        if( pMem==MAP_FAILED ){
          rc = unixLogError(SQLITE_IOERR_SHMMAP, "mmap", pShmNode->zFilename);
          goto shmpage_out;
        }
      }else{
        pMem = sqlite3_malloc(szRegion);
        if( pMem==0 ){
          rc = SQLITE_NOMEM;
          goto shmpage_out;
        }
        memset(pMem, 0, szRegion);
      }
      pShmNode->apRegion[pShmNode->nRegion] = pMem;
      pShmNode->nRegion++;
    }
  }

shmpage_out:
  if( pShmNode->nRegion>iRegion ){
    *pp = pShmNode->apRegion[iRegion];
  }else{
    *pp = 0;
  }
  if( pShmNode->isReadonly && rc==SQLITE_OK ) rc = SQLITE_READONLY;
  sqlite3_mutex_leave(pShmNode->mutex);
  return rc;
}

/*
** Change the lock state for a shared-memory segment.
**
** Note that the relationship between SHAREd and EXCLUSIVE locks is a little
** different here than in posix.  In xShmLock(), one can go from unlocked
** to shared and back or from unlocked to exclusive and back.  But one may
** not go from shared to exclusive or from exclusive to shared.
**
** 更改共享内存段的锁定状态。
**
** 请注意，在此处SHAREd和EXCLUSIVE锁之间的关系和posix不同。
** 在xShmLock（）中，可以实现从解锁到共享和从解锁到独占
** 和返回之间的转换。但是不能实现从共享到独占或从独占到共享的转换。
**
*/
static int unixShmLock(
  sqlite3_file *fd,          /* Database file holding the shared memory */
  int ofst,                  /* First lock to acquire or release */
  int n,                     /* Number of locks to acquire or release */
  int flags                  /* What to do with the lock */
){
  unixFile *pDbFd = (unixFile*)fd;      /* Connection holding shared memory */
  unixShm *p = pDbFd->pShm;             /* The shared memory being locked */
  unixShm *pX;                          /* For looping over all siblings */
  unixShmNode *pShmNode = p->pShmNode;  /* The underlying file iNode */
  int rc = SQLITE_OK;                   /* Result code */
  u16 mask;                             /* Mask of locks to take or release */

  assert( pShmNode==pDbFd->pInode->pShmNode );
  assert( pShmNode->pInode==pDbFd->pInode );
  assert( ofst>=0 && ofst+n<=SQLITE_SHM_NLOCK );
  assert( n>=1 );
  assert( flags==(SQLITE_SHM_LOCK | SQLITE_SHM_SHARED)
       || flags==(SQLITE_SHM_LOCK | SQLITE_SHM_EXCLUSIVE)
       || flags==(SQLITE_SHM_UNLOCK | SQLITE_SHM_SHARED)
       || flags==(SQLITE_SHM_UNLOCK | SQLITE_SHM_EXCLUSIVE) );
  assert( n==1 || (flags & SQLITE_SHM_EXCLUSIVE)!=0 );
  assert( pShmNode->h>=0 || pDbFd->pInode->bProcessLock==1 );
  assert( pShmNode->h<0 || pDbFd->pInode->bProcessLock==0 );

  mask = (1<<(ofst+n)) - (1<<ofst);
  assert( n>1 || mask==(1<<ofst) );
  sqlite3_mutex_enter(pShmNode->mutex);
  if( flags & SQLITE_SHM_UNLOCK ){
    u16 allMask = 0; /* Mask of locks held by siblings */

    /* See if any siblings hold this same lock */
    for(pX=pShmNode->pFirst; pX; pX=pX->pNext){
      if( pX==p ) continue;
      assert( (pX->exclMask & (p->exclMask|p->sharedMask))==0 );
      allMask |= pX->sharedMask;
    }

    /* Unlock the system-level locks */
    if( (mask & allMask)==0 ){
      rc = unixShmSystemLock(pShmNode, F_UNLCK, ofst+UNIX_SHM_BASE, n);
    }else{
      rc = SQLITE_OK;
    }

    /* Undo the local locks */
    if( rc==SQLITE_OK ){
      p->exclMask &= ~mask;
      p->sharedMask &= ~mask;
    } 
  }else if( flags & SQLITE_SHM_SHARED ){
    u16 allShared = 0;  /* Union of locks held by connections other than "p" */

    /* Find out which shared locks are already held by sibling connections.
    ** If any sibling already holds an exclusive lock, go ahead and return
    ** SQLITE_BUSY.
    ** 查找哪些共享锁已经由兄弟连接持有。如果有同级进程占有独占锁，
    ** 请继续并返回SQLITE_BUSY。
    */
    for(pX=pShmNode->pFirst; pX; pX=pX->pNext){
      if( (pX->exclMask & mask)!=0 ){
        rc = SQLITE_BUSY;
        break;
      }
      allShared |= pX->sharedMask;
    }

    /* Get shared locks at the system level, if necessary */
    if( rc==SQLITE_OK ){
      if( (allShared & mask)==0 ){
        rc = unixShmSystemLock(pShmNode, F_RDLCK, ofst+UNIX_SHM_BASE, n);
      }else{
        rc = SQLITE_OK;
      }
    }

    /* Get the local shared locks */
    if( rc==SQLITE_OK ){
      p->sharedMask |= mask;
    }
  }else{
    /* Make sure no sibling connections hold locks that will block this
    ** lock.  If any do, return SQLITE_BUSY right away.
    **
    ** 确保没有同级连接拥有将阻止此锁的锁。 如果有，返回值为SQLITE_BUSY。
    */
    for(pX=pShmNode->pFirst; pX; pX=pX->pNext){
      if( (pX->exclMask & mask)!=0 || (pX->sharedMask & mask)!=0 ){
        rc = SQLITE_BUSY;
        break;
      }
    }
  
    /* Get the exclusive locks at the system level.  Then if successful
    ** also mark the local connection as being locked.
    ** 获取系统级排它锁。 成功就将本地连接标记为已锁定。
    */
    if( rc==SQLITE_OK ){
      rc = unixShmSystemLock(pShmNode, F_WRLCK, ofst+UNIX_SHM_BASE, n);
      if( rc==SQLITE_OK ){
        assert( (p->sharedMask & mask)==0 );
        p->exclMask |= mask;
      }
    }
  }
  sqlite3_mutex_leave(pShmNode->mutex);
  OSTRACE(("SHM-LOCK shmid-%d, pid-%d got %03x,%03x\n",
           p->id, getpid(), p->sharedMask, p->exclMask));
  return rc;
}

/*
** Implement a memory barrier or memory fence on shared memory.  
**
** All loads and stores begun before the barrier must complete before
** any load or store begun after the barrier.
*/
static void unixShmBarrier(
  sqlite3_file *fd                /* Database file holding the shared memory */
){
  UNUSED_PARAMETER(fd);
  unixEnterMutex();
  unixLeaveMutex();
}

/*
** Close a connection to shared-memory.  Delete the underlying 
** storage if deleteFlag is true.
**
** If there is no shared memory associated with the connection then this
** routine is a harmless no-op.
*/
static int unixShmUnmap(
  sqlite3_file *fd,               /* The underlying database file */
  int deleteFlag                  /* Delete shared-memory if true */
){
  unixShm *p;                     /* The connection to be closed */
  unixShmNode *pShmNode;          /* The underlying shared-memory file */
  unixShm **pp;                   /* For looping over sibling connections */
  unixFile *pDbFd;                /* The underlying database file */

  pDbFd = (unixFile*)fd;
  p = pDbFd->pShm;
  if( p==0 ) return SQLITE_OK;
  pShmNode = p->pShmNode;

  assert( pShmNode==pDbFd->pInode->pShmNode );
  assert( pShmNode->pInode==pDbFd->pInode );

  /* Remove connection p from the set of connections associated
  ** with pShmNode */
  sqlite3_mutex_enter(pShmNode->mutex);
  for(pp=&pShmNode->pFirst; (*pp)!=p; pp = &(*pp)->pNext){}
  *pp = p->pNext;

  /* Free the connection p */
  sqlite3_free(p);
  pDbFd->pShm = 0;
  sqlite3_mutex_leave(pShmNode->mutex);

  /* If pShmNode->nRef has reached 0, then close the underlying
  ** shared-memory file, too */
  unixEnterMutex();
  assert( pShmNode->nRef>0 );
  pShmNode->nRef--;
  if( pShmNode->nRef==0 ){
    if( deleteFlag && pShmNode->h>=0 ) osUnlink(pShmNode->zFilename);
    unixShmPurge(pDbFd);
  }
  unixLeaveMutex();

  return SQLITE_OK;
}


#else
# define unixShmMap     0
# define unixShmLock    0
# define unixShmBarrier 0
# define unixShmUnmap   0
#endif /* #ifndef SQLITE_OMIT_WAL */

/*
** Here ends the implementation of all sqlite3_file methods.
**
********************** End sqlite3_file Methods *******************************
******************************************************************************/

/*
** This division contains definitions of sqlite3_io_methods objects that
** implement various file locking strategies.  It also contains definitions
** of "finder" functions.  A finder-function is used to locate the appropriate
** sqlite3_io_methods object for a particular database file.  The pAppData
** field of the sqlite3_vfs VFS objects are initialized to be pointers to
** the correct finder-function for that VFS.
**
** Most finder functions return a pointer to a fixed sqlite3_io_methods
** object.  The only interesting finder-function is autolockIoFinder, which
** looks at the filesystem type and tries to guess the best locking
** strategy from that.
** 
** 此分区包含实现各种文件锁定策略的sqlite3_io_methods对象定义。 
** 还包含“finder”函数的定义。 finder函数用于为特定数据库文件
** 定位到适当的sqlite3_io_methods对象。 sqlite3_vfs VFS对象的pAppData字段
** 被初始化为指向该VFS的正确finder函数的指针。
**
** 大多数finder函数返回一个指向sqlite3_io_methods对象固定区域的指针。
** 有趣的是finder函数是autolockIoFinder，它查看文件系统类型，并尝试
** 从中得到最佳锁定策略。
**
** For finder-funtion F, two objects are created:
**
**    (1) The real finder-function named "FImpt()".
**
**    (2) A constant pointer to this function named just "F".
**
**
** A pointer to the F pointer is used as the pAppData value for VFS
** objects.  We have to do this instead of letting pAppData point
** directly at the finder-function since C90 rules prevent a void*
** from be cast into a function pointer.
**
**
** Each instance of this macro generates two objects:
**
**   *  A constant sqlite3_io_methods object call METHOD that has locking
**      methods CLOSE, LOCK, UNLOCK, CKRESLOCK.
**
**   *  An I/O method finder function called FINDER that returns a pointer
**      to the METHOD object in the previous bullet.
**
** 对于finder-funtion F，创建两个对象：
**
**（1）名为“FImpt()”的finder函数。
**
**（2）这个函数的常量指针，命名为“F”。
**
**
** 一个指向指针F的指针用作VFS对象的pAppData值.我们必须这样做，
** 而不是让pAppData直接指向finder函数，因为C90规则阻止将void *转换为函数指针。
**
** 这个宏的每个实例都会生成两个对象：
**  *常量sqlite3_io_methods对象调用METHOD有锁定方法CLOSE，LOCK，UNLOCK，CKRESLOCK。
**
**  *FINDER的I / O方法查找器函数，返回一个指向上一个项目符号中的METHOD对象的指针。
**
*/
#define IOMETHODS(FINDER, METHOD, VERSION, CLOSE, LOCK, UNLOCK, CKLOCK)      \
static const sqlite3_io_methods METHOD = {                                   \
   VERSION,                    /* iVersion */                                \
   CLOSE,                      /* xClose */                                  \
   unixRead,                   /* xRead */                                   \
   unixWrite,                  /* xWrite */                                  \
   unixTruncate,               /* xTruncate */                               \
   unixSync,                   /* xSync */                                   \
   unixFileSize,               /* xFileSize */                               \
   LOCK,                       /* xLock */                                   \
   UNLOCK,                     /* xUnlock */                                 \
   CKLOCK,                     /* xCheckReservedLock */                      \
   unixFileControl,            /* xFileControl */                            \
   unixSectorSize,             /* xSectorSize */                             \
   unixDeviceCharacteristics,  /* xDeviceCapabilities */                     \
   unixShmMap,                 /* xShmMap */                                 \
   unixShmLock,                /* xShmLock */                                \
   unixShmBarrier,             /* xShmBarrier */                             \
   unixShmUnmap                /* xShmUnmap */                               \
};                                                                           \
static const sqlite3_io_methods *FINDER##Impl(const char *z, unixFile *p){   \
  UNUSED_PARAMETER(z); UNUSED_PARAMETER(p);                                  \
  return &METHOD;                                                            \
}                                                                            \
static const sqlite3_io_methods *(*const FINDER)(const char*,unixFile *p)    \
    = FINDER##Impl;

/*
** Here are all of the sqlite3_io_methods objects for each of the
** locking strategies.  Functions that return pointers to these methods
** are also created.
**
** 下面是每个锁定策略的所有sqlite3_io_methods对象。
** 创建之后返回指向这些方法的指针。
**
*/
IOMETHODS(
  posixIoFinder,            /* Finder function name */
  posixIoMethods,           /* sqlite3_io_methods object name */
  2,                        /* shared memory is enabled */
  unixClose,                /* xClose method */
  unixLock,                 /* xLock method */
  unixUnlock,               /* xUnlock method */
  unixCheckReservedLock     /* xCheckReservedLock method */
)
IOMETHODS(
  nolockIoFinder,           /* Finder function name */
  nolockIoMethods,          /* sqlite3_io_methods object name */
  1,                        /* shared memory is disabled */
  nolockClose,              /* xClose method */
  nolockLock,               /* xLock method */
  nolockUnlock,             /* xUnlock method */
  nolockCheckReservedLock   /* xCheckReservedLock method */
)
IOMETHODS(
  dotlockIoFinder,          /* Finder function name */
  dotlockIoMethods,         /* sqlite3_io_methods object name */
  1,                        /* shared memory is disabled */
  dotlockClose,             /* xClose method */
  dotlockLock,              /* xLock method */
  dotlockUnlock,            /* xUnlock method */
  dotlockCheckReservedLock  /* xCheckReservedLock method */
)

#if SQLITE_ENABLE_LOCKING_STYLE && !OS_VXWORKS
IOMETHODS(
  flockIoFinder,            /* Finder function name */
  flockIoMethods,           /* sqlite3_io_methods object name */
  1,                        /* shared memory is disabled */
  flockClose,               /* xClose method */
  flockLock,                /* xLock method */
  flockUnlock,              /* xUnlock method */
  flockCheckReservedLock    /* xCheckReservedLock method */
)
#endif

#if OS_VXWORKS
IOMETHODS(
  semIoFinder,              /* Finder function name */
  semIoMethods,             /* sqlite3_io_methods object name */
  1,                        /* shared memory is disabled */
  semClose,                 /* xClose method */
  semLock,                  /* xLock method */
  semUnlock,                /* xUnlock method */
  semCheckReservedLock      /* xCheckReservedLock method */
)
#endif

#if defined(__APPLE__) && SQLITE_ENABLE_LOCKING_STYLE
IOMETHODS(
  afpIoFinder,              /* Finder function name */
  afpIoMethods,             /* sqlite3_io_methods object name */
  1,                        /* shared memory is disabled */
  afpClose,                 /* xClose method */
  afpLock,                  /* xLock method */
  afpUnlock,                /* xUnlock method */
  afpCheckReservedLock      /* xCheckReservedLock method */
)
#endif

/*
** The proxy locking method is a "super-method" in the sense that it
** opens secondary file descriptors for the conch and lock files and
** it uses proxy, dot-file, AFP, and flock() locking methods on those
** secondary files.  For this reason, the division that implements
** proxy locking is located much further down in the file.  But we need
** to go ahead and define the sqlite3_io_methods and finder function
** for proxy locking here.  So we forward declare the I/O methods.
**
** 代理锁定方法是一个“超级方法”，它打开conch和锁定文件的
** 辅助文件描述符，在这些辅助文件上使用proxy，dot文件，AFP和
** flock()锁定方法。 出于这个原因，由文件层的底层代码实现锁代理。
** 但是我们需要在这里为锁代理定义sqlite3_io_methods和finder函数。
** 因此我们在这里进行I / O方法前向声明。

*/
#if defined(__APPLE__) && SQLITE_ENABLE_LOCKING_STYLE
static int proxyClose(sqlite3_file*);
static int proxyLock(sqlite3_file*, int);
static int proxyUnlock(sqlite3_file*, int);
static int proxyCheckReservedLock(sqlite3_file*, int*);
IOMETHODS(
  proxyIoFinder,            /* Finder function name */
  proxyIoMethods,           /* sqlite3_io_methods object name */
  1,                        /* shared memory is disabled */
  proxyClose,               /* xClose method */
  proxyLock,                /* xLock method */
  proxyUnlock,              /* xUnlock method */
  proxyCheckReservedLock    /* xCheckReservedLock method */
)
#endif

/* nfs lockd on OSX 10.3+ doesn't clear write locks when a read lock is set */
#if defined(__APPLE__) && SQLITE_ENABLE_LOCKING_STYLE
IOMETHODS(
  nfsIoFinder,               /* Finder function name */
  nfsIoMethods,              /* sqlite3_io_methods object name */
  1,                         /* shared memory is disabled */
  unixClose,                 /* xClose method */
  unixLock,                  /* xLock method */
  nfsUnlock,                 /* xUnlock method */
  unixCheckReservedLock      /* xCheckReservedLock method */
)
#endif

#if defined(__APPLE__) && SQLITE_ENABLE_LOCKING_STYLE
/* 
** This "finder" function attempts to determine the best locking strategy 
** for the database file "filePath".  It then returns the sqlite3_io_methods
** object that implements that strategy.
**
** This is for MacOSX only.
**
** 这个“finder”函数尝试确定数据库文件“filePath”的最佳锁策略。 
** 然后返回实现该策略的sqlite3_io_methods对象。
** 但是这仅适用于MacOSX平台。
**
*/
static const sqlite3_io_methods *autolockIoFinderImpl(
  const char *filePath,    /* name of the database file */
  unixFile *pNew           /* open file object for the database file */
){
  static const struct Mapping {
    const char *zFilesystem;              /* Filesystem type name */
    const sqlite3_io_methods *pMethods;   /* Appropriate locking method */
  } aMap[] = {
    { "hfs",    &posixIoMethods },
    { "ufs",    &posixIoMethods },
    { "afpfs",  &afpIoMethods },
    { "smbfs",  &afpIoMethods },
    { "webdav", &nolockIoMethods },
    { 0, 0 }
  };
  int i;
  struct statfs fsInfo;
  struct flock lockInfo;

  if( !filePath ){
    /* If filePath==NULL that means we are dealing with a transient file
    ** that does not need to be locked. */
    return &nolockIoMethods;
  }
  if( statfs(filePath, &fsInfo) != -1 ){
    if( fsInfo.f_flags & MNT_RDONLY ){
      return &nolockIoMethods;
    }
    for(i=0; aMap[i].zFilesystem; i++){
      if( strcmp(fsInfo.f_fstypename, aMap[i].zFilesystem)==0 ){
        return aMap[i].pMethods;
      }
    }
  }

  /* Default case. Handles, amongst others, "nfs".
  ** Test byte-range lock using fcntl(). If the call succeeds, 
  ** assume that the file-system supports POSIX style locks. 
  **
  ** 默认情况下，所有句柄，包括“nfs”句柄。
  ** 使用fcntl()测试用于锁定的字节范围。 如果调用成功，
  ** 则假定文件系统支持POSIX锁。
  */
  lockInfo.l_len = 1;
  lockInfo.l_start = 0;
  lockInfo.l_whence = SEEK_SET;
  lockInfo.l_type = F_RDLCK;
  if( osFcntl(pNew->h, F_GETLK, &lockInfo)!=-1 ) {
    if( strcmp(fsInfo.f_fstypename, "nfs")==0 ){
      return &nfsIoMethods;
    } else {
      return &posixIoMethods;
    }
  }else{
    return &dotlockIoMethods;
  }
}
static const sqlite3_io_methods 
  *(*const autolockIoFinder)(const char*,unixFile*) = autolockIoFinderImpl;

#endif /* defined(__APPLE__) && SQLITE_ENABLE_LOCKING_STYLE */

#if OS_VXWORKS && SQLITE_ENABLE_LOCKING_STYLE
/* 
** This "finder" function attempts to determine the best locking strategy 
** for the database file "filePath".  It then returns the sqlite3_io_methods
** object that implements that strategy.
**
** This is for VXWorks only.
**
** “finder”函数试图确定数据库文件“filePath”的最佳锁定策略。 
** 返回值为实现该策略的sqlite3_io_methods对象。
** 
** 这仅适用于VXWorks平台。
**
*/
static const sqlite3_io_methods *autolockIoFinderImpl(
  const char *filePath,    /* name of the database file */
  unixFile *pNew           /* the open file object */
){
  struct flock lockInfo;

  if( !filePath ){
    /* If filePath==NULL that means we are dealing with a transient file
    ** that does not need to be locked. */
    return &nolockIoMethods;
  }

  /* Test if fcntl() is supported and use POSIX style locks.
  ** Otherwise fall back to the named semaphore method.
  */
  lockInfo.l_len = 1;
  lockInfo.l_start = 0;
  lockInfo.l_whence = SEEK_SET;
  lockInfo.l_type = F_RDLCK;
  if( osFcntl(pNew->h, F_GETLK, &lockInfo)!=-1 ) {
    return &posixIoMethods;
  }else{
    return &semIoMethods;
  }
}
static const sqlite3_io_methods 
  *(*const autolockIoFinder)(const char*,unixFile*) = autolockIoFinderImpl;

#endif /* OS_VXWORKS && SQLITE_ENABLE_LOCKING_STYLE */

/*
** An abstract type for a pointer to a IO method finder function:
*/
typedef const sqlite3_io_methods *(*finder_type)(const char*,unixFile*);


/****************************************************************************
**************************** sqlite3_vfs methods ****************************
**
** This division contains the implementation of methods on the
** sqlite3_vfs object.
** 
** 这个部分包含实现sqlite3_vfs对象的方法。
*/

/*
** Initialize the contents of the unixFile structure pointed to by pId.
**
** 初始化指向unixFile结构的pId。
**
*/
static int fillInUnixFile(
  sqlite3_vfs *pVfs,      /* Pointer to vfs object */
  int h,                  /* Open file descriptor of file being opened */
  sqlite3_file *pId,      /* Write to the unixFile structure here */
  const char *zFilename,  /* Name of the file being opened */
  int ctrlFlags           /* Zero or more UNIXFILE_* values */
){
  const sqlite3_io_methods *pLockingStyle;
  unixFile *pNew = (unixFile *)pId;
  int rc = SQLITE_OK;

  assert( pNew->pInode==NULL );

  /* Usually the path zFilename should not be a relative pathname. The
  ** exception is when opening the proxy "conch" file in builds that
  ** include the special Apple locking styles.
  **
  ** 通常情况下路径zFilename不应该是相对路径名。唯一的例外是在Apple风格的锁机制中
  ** 创建时打开"conch"代理文件。
  */
#if defined(__APPLE__) && SQLITE_ENABLE_LOCKING_STYLE
  assert( zFilename==0 || zFilename[0]=='/' 
    || pVfs->pAppData==(void*)&autolockIoFinder );
#else
  assert( zFilename==0 || zFilename[0]=='/' );
#endif

  /* No locking occurs in temporary files */
  assert( zFilename!=0 || (ctrlFlags & UNIXFILE_NOLOCK)!=0 );

  OSTRACE(("OPEN    %-3d %s\n", h, zFilename));
  pNew->h = h;
  pNew->pVfs = pVfs;
  pNew->zPath = zFilename;
  pNew->ctrlFlags = (u8)ctrlFlags;
  if( sqlite3_uri_boolean(((ctrlFlags & UNIXFILE_URI) ? zFilename : 0),
                           "psow", SQLITE_POWERSAFE_OVERWRITE) ){
    pNew->ctrlFlags |= UNIXFILE_PSOW;
  }
  if( memcmp(pVfs->zName,"unix-excl",10)==0 ){
    pNew->ctrlFlags |= UNIXFILE_EXCL;
  }

#if OS_VXWORKS
  pNew->pId = vxworksFindFileId(zFilename);
  if( pNew->pId==0 ){
    ctrlFlags |= UNIXFILE_NOLOCK;
    rc = SQLITE_NOMEM;
  }
#endif

  if( ctrlFlags & UNIXFILE_NOLOCK ){
    pLockingStyle = &nolockIoMethods;
  }else{
    pLockingStyle = (**(finder_type*)pVfs->pAppData)(zFilename, pNew);
#if SQLITE_ENABLE_LOCKING_STYLE
    /* Cache zFilename in the locking context (AFP and dotlock override) for
    ** proxyLock activation is possible (remote proxy is based on db name)
    ** zFilename remains valid until file is closed, to support 
    ** 
    ** 为实现在锁定上下文（AFP和dotlock覆盖）中对缓存的zFilename
    ** 进行proxyLock激活，（基于数据库名称实现远程代理）zFilename在文件
    ** 关闭之前保持.
    ** 
    */
    pNew->lockingContext = (void*)zFilename;
#endif
  }

  if( pLockingStyle == &posixIoMethods
#if defined(__APPLE__) && SQLITE_ENABLE_LOCKING_STYLE
    || pLockingStyle == &nfsIoMethods
#endif
  ){
    unixEnterMutex();
    rc = findInodeInfo(pNew, &pNew->pInode);
    if( rc!=SQLITE_OK ){
      /* If an error occured in findInodeInfo(), close the file descriptor
      ** immediately, before releasing the mutex. findInodeInfo() may fail
      ** in two scenarios:
      **
      **   (a) A call to fstat() failed.
      **   (b) A malloc failed.
      **
      ** Scenario (b) may only occur if the process is holding no other
      ** file descriptors open on the same file. If there were other file
      ** descriptors on this file, then no malloc would be required by
      ** findInodeInfo(). If this is the case, it is quite safe to close
      ** handle h - as it is guaranteed that no posix locks will be released
      ** by doing so.
      **
      ** If scenario (a) caused the error then things are not so safe. The
      ** implicit assumption here is that if fstat() fails, things are in
      ** such bad shape that dropping a lock or two doesn't matter much.
      **
      ** 如果在findInodeInfo（）中发生错误，则在释放互斥体之前
      ** 立即关闭文件描述符。findInodeInfo（）可能会在这两种情况下失败：
      **
      **（a）对fstat（）的调用失败。
      **（b）malloc失败。
      **
      ** 方案（b）可能仅在进程没有在同一文件上保持其他文件描述符
      ** 打开时发生。如果此文件上还有其他文件描述符，那么findInodeInfo()函数
      ** 就不需要malloc。在这种情况下，关闭句柄是相当安全的，因为它保证
      ** 不会这样做释放posix锁。
      **
      ** 如果情况（a）导致错误，那么操作就不那么安全。这里的隐含假设是，
      ** 如果fstat（）失败，既然事情已经这样糟糕，丢弃一两个锁也无关紧要。
      */
      robust_close(pNew, h, __LINE__);
      h = -1;
    }
    unixLeaveMutex();
  }

#if SQLITE_ENABLE_LOCKING_STYLE && defined(__APPLE__)
  else if( pLockingStyle == &afpIoMethods ){
    /* AFP locking uses the file path so it needs to be included in
    ** the afpLockingContext.
    **
    ** AFP锁需要使用文件路径，因此它需要包含在afpLockingContext中。
    */
    afpLockingContext *pCtx;
    pNew->lockingContext = pCtx = sqlite3_malloc( sizeof(*pCtx) );
    if( pCtx==0 ){
      rc = SQLITE_NOMEM;
    }else{
      /* NB: zFilename exists and remains valid until the file is closed
      ** according to requirement F11141.  So we do not need to make a
      ** copy of the filename. 
      **
      ** 注意：zFilename存在并始终保持有效，直到根据F11141要求
      ** 关闭文件。 所以我们不需要复制文件名。
      **
      */
      pCtx->dbPath = zFilename;
      pCtx->reserved = 0;
      srandomdev();
      unixEnterMutex();
      rc = findInodeInfo(pNew, &pNew->pInode);
      if( rc!=SQLITE_OK ){
        sqlite3_free(pNew->lockingContext);
        robust_close(pNew, h, __LINE__);
        h = -1;
      }
      unixLeaveMutex();        
    }
  }
#endif

  else if( pLockingStyle == &dotlockIoMethods ){
    /* Dotfile locking uses the file path so it needs to be included in
    ** the dotlockLockingContext 
    **
    ** Dotfile锁需要使用文件路径，因此需要包含在dotlockLockingContext中
    */
    char *zLockFile;
    int nFilename;
    assert( zFilename!=0 );
    nFilename = (int)strlen(zFilename) + 6;
    zLockFile = (char *)sqlite3_malloc(nFilename);
    if( zLockFile==0 ){
      rc = SQLITE_NOMEM;
    }else{
      sqlite3_snprintf(nFilename, zLockFile, "%s" DOTLOCK_SUFFIX, zFilename);
    }
    pNew->lockingContext = zLockFile;
  }

#if OS_VXWORKS
  else if( pLockingStyle == &semIoMethods ){
    /* Named semaphore locking uses the file path so it needs to be
    ** included in the semLockingContext
    **
    ** 信号量标记锁需要使用文件路径，因此它需要包含在semLockingContext中
    */
    unixEnterMutex();
    rc = findInodeInfo(pNew, &pNew->pInode);
    if( (rc==SQLITE_OK) && (pNew->pInode->pSem==NULL) ){
      char *zSemName = pNew->pInode->aSemName;
      int n;
      sqlite3_snprintf(MAX_PATHNAME, zSemName, "/%s.sem",
                       pNew->pId->zCanonicalName);
      for( n=1; zSemName[n]; n++ )
        if( zSemName[n]=='/' ) zSemName[n] = '_';
      pNew->pInode->pSem = sem_open(zSemName, O_CREAT, 0666, 1);
      if( pNew->pInode->pSem == SEM_FAILED ){
        rc = SQLITE_NOMEM;
        pNew->pInode->aSemName[0] = '\0';
      }
    }
    unixLeaveMutex();
  }
#endif
  
  pNew->lastErrno = 0;
#if OS_VXWORKS
  if( rc!=SQLITE_OK ){
    if( h>=0 ) robust_close(pNew, h, __LINE__);
    h = -1;
    osUnlink(zFilename);
    isDelete = 0;
  }
  if( isDelete ) pNew->ctrlFlags |= UNIXFILE_DELETE;
#endif
  if( rc!=SQLITE_OK ){
    if( h>=0 ) robust_close(pNew, h, __LINE__);
  }else{
    pNew->pMethod = pLockingStyle;
    OpenCounter(+1);
  }
  return rc;
}

/*
** Return the name of a directory in which to put temporary files.
** If no suitable temporary file directory can be found, return NULL.
** 
** 返回存放临时文件的目录名称。如果没有找到合适的
** 临时文件目录，返回值为NULL。
*/
static const char *unixTempFileDir(void){
  static const char *azDirs[] = {
     0,
     0,
     "/var/tmp",
     "/usr/tmp",
     "/tmp",
     0        /* List terminator */
  };
  unsigned int i;
  struct stat buf;
  const char *zDir = 0;

  azDirs[0] = sqlite3_temp_directory;
  if( !azDirs[1] ) azDirs[1] = getenv("TMPDIR");
  for(i=0; i<sizeof(azDirs)/sizeof(azDirs[0]); zDir=azDirs[i++]){
    if( zDir==0 ) continue;
    if( osStat(zDir, &buf) ) continue;
    if( !S_ISDIR(buf.st_mode) ) continue;
    if( osAccess(zDir, 07) ) continue;
    break;
  }
  return zDir;
}

/*
** Create a temporary file name in zBuf.  zBuf must be allocated
** by the calling process and must be big enough to hold at least
** pVfs->mxPathname bytes.
**
** 在zBuf中创建临时文件名。 zBuf大小必须由调用进程分配，
** 且必须大到存放所有pVfs-> mxPathname字节。
**
*/
static int unixGetTempname(int nBuf, char *zBuf){
  static const unsigned char zChars[] =
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789";
  unsigned int i, j;
  const char *zDir;

  /* It's odd to simulate an io-error here, but really this is just
  ** using the io-error infrastructure to test that SQLite handles this
  ** function failing. 
  **
  ** 在这里模拟一个io错误看起来有点奇怪，但实际上这只是
  ** 使用这个io错误来测试SQLite对于失败函数的处理。
  */
  SimulateIOError( return SQLITE_IOERR );

  zDir = unixTempFileDir();
  if( zDir==0 ) zDir = ".";

  /* Check that the output buffer is large enough for the temporary file 
  ** name. If it is not, return SQLITE_ERROR.
  ** 
  ** 检查输出缓冲区是否可以存放临时文件名。
  ** 如果不可以，返回值为SQLITE_ERROR。
  */
  if( (strlen(zDir) + strlen(SQLITE_TEMP_FILE_PREFIX) + 18) >= (size_t)nBuf ){
    return SQLITE_ERROR;
  }

  do{
    sqlite3_snprintf(nBuf-18, zBuf, "%s/"SQLITE_TEMP_FILE_PREFIX, zDir);
    j = (int)strlen(zBuf);
    sqlite3_randomness(15, &zBuf[j]);
    for(i=0; i<15; i++, j++){
      zBuf[j] = (char)zChars[ ((unsigned char)zBuf[j])%(sizeof(zChars)-1) ];
    }
    zBuf[j] = 0;
    zBuf[j+1] = 0;
  }while( osAccess(zBuf,0)==0 );
  return SQLITE_OK;
}

#if SQLITE_ENABLE_LOCKING_STYLE && defined(__APPLE__)
/*
** Routine to transform a unixFile into a proxy-locking unixFile.
** Implementation in the proxy-lock division, but used by unixOpen()
** if SQLITE_PREFER_PROXY_LOCKING is defined.
**
** 将unixFile转换为unixFile代理锁。在代理锁分区中实现.
** 如果定义了SQLITE_PREFER_PROXY_LOCKING，就由unixOpen()使用.
**
*/
static int proxyTransformUnixFile(unixFile*, const char*);
#endif

/*
** Search for an unused file descriptor that was opened on the database 
** file (not a journal or master-journal file) identified by pathname
** zPath with SQLITE_OPEN_XXX flags matching those passed as the second
** argument to this function.
**
** Such a file descriptor may exist if a database connection was closed
** but the associated file descriptor could not be closed because some
** other file descriptor open on the same file is holding a file-lock.
** Refer to comments in the unixClose() function and the lengthy comment
** describing "Posix Advisory Locking" at the start of this file for 
** further details. Also, ticket #4018.
**
** If a suitable file descriptor is found, then it is returned. If no
** such file descriptor is located, -1 is returned.
**
** 在数据库文件（而不是日志或主日志文件）搜索路径名为zPath标识的
** 已打开未使用的文件描述符，且将SQLITE_OPEN_XXX标志与作为此函数的
** 第二个参数传递的标志相匹配。
**
** 这样的文件描述符可能存在，如果数据库连接关闭，但相关的文件描述符
** 无法关闭，因为在同一个文件上打开的一些其他文件描述符持有文件锁。
** 请参阅unixClose（）函数中的注释和关于“Posix Advisory Locking”的全部注释，
** 从而获取更多详细信息。 此外，可以参考＃4018。
**
** 如果找到合适的文件描述符，则返回这个文件描述符。 如果没有找到
** 这样的文件描述符，则返回-1。
**
*/
static UnixUnusedFd *findReusableFd(const char *zPath, int flags){
  UnixUnusedFd *pUnused = 0;

  /* Do not search for an unused file descriptor on vxworks. Not because
  ** vxworks would not benefit from the change (it might, we're not sure),
  ** but because no way to test it is currently available. It is better 
  ** not to risk breaking vxworks support for the sake of such an obscure 
  ** feature.  
  **
  ** 不要在vxworks上搜索未使用的文件描述符。不是因为vxworks不会从改变中
  ** 得到某种好处（它可能，我们不确定），但是在目前没有可用的方法进行测试。
  ** 所以最好不要为这样一个模糊的功能而破坏原始的vxworks函数。
  ** 
  */
#if !OS_VXWORKS
  struct stat sStat;                   /* Results of stat() call */

  /* A stat() call may fail for various reasons. If this happens, it is
  ** almost certain that an open() call on the same path will also fail.
  ** For this reason, if an error occurs in the stat() call here, it is
  ** ignored and -1 is returned. The caller will try to open a new file
  ** descriptor on the same path, fail, and return an error to SQLite.
  **
  ** Even if a subsequent open() call does succeed, the consequences of
  ** not searching for a resusable file descriptor are not dire.  
  **
  ** stat()的调用可能由于各种原因失败。如果发生这种情况，几乎可以肯定的是
  ** 在同一路径上的open()调用也会失败。因此，如果在stat()调用中发生错误，则
  ** 忽略该错误，并返回-1。 调用者将尝试在同一路径上打开一个新的文件描述符，
  ** 很大可能会失败，则返回一个错误给SQLite。
  **
  ** 即使随后的open（）调用确实成功，不搜索可重用文件描述符的后果也不可怕。
  **
  */
  if( 0==osStat(zPath, &sStat) ){
    unixInodeInfo *pInode;

    unixEnterMutex();
    pInode = inodeList;
    while( pInode && (pInode->fileId.dev!=sStat.st_dev
                     || pInode->fileId.ino!=sStat.st_ino) ){
       pInode = pInode->pNext;
    }
    if( pInode ){
      UnixUnusedFd **pp;
      for(pp=&pInode->pUnused; *pp && (*pp)->flags!=flags; pp=&((*pp)->pNext));
      pUnused = *pp;
      if( pUnused ){
        *pp = pUnused->pNext;
      }
    }
    unixLeaveMutex();
  }
#endif    /* if !OS_VXWORKS */
  return pUnused;
}

/*
** This function is called by unixOpen() to determine the unix permissions
** to create new files with. If no error occurs, then SQLITE_OK is returned
** and a value suitable for passing as the third argument to open(2) is
** written to *pMode. If an IO error occurs, an SQLite error code is 
** returned and the value of *pMode is not modified.
**
** In most cases cases, this routine sets *pMode to 0, which will become
** an indication to robust_open() to create the file using
** SQLITE_DEFAULT_FILE_PERMISSIONS adjusted by the umask.
** But if the file being opened is a WAL or regular journal file, then 
** this function queries the file-system for the permissions on the 
** corresponding database file and sets *pMode to this value. Whenever 
** possible, WAL and journal files are created using the same permissions 
** as the associated database file.
**
** If the SQLITE_ENABLE_8_3_NAMES option is enabled, then the
** original filename is unavailable.  But 8_3_NAMES is only used for
** FAT filesystems and permissions do not matter there, so just use
** the default permissions.
** 
** 此函数由unixOpen()调用从而确定用于创建新文件的unix权限。
** 如果没有发生错误，返回值为SQLITE_OK，并将适合的值作为第三个参数
** 传递给open(2)写入* pMode。如果发生IO错误，则返回SQLite错误代码，且不
** 修改* pMode的值。
**
** 在大多数情况下，此例程将*pMode设置为0，这就是调用robust_open（）的标志，
** 使用由umask调整的SQLITE_DEFAULT_FILE_PERMISSIONS创建文件。但是，如果要打开的
** 文件是WAL或常规的日志文件，此函数将查询文件系统从而获取相应数据库
** 文件的权限，并将*pMode设置为该值。可能的情况下，就使用和对应数据库文件
** 相同的权限创建WAL和日志文件。
**
** 如果启用了SQLITE_ENABLE_8_3_NAMES选项，则原始文件名不可用。但8_3_NAMES只用于
** FAT文件系统，隐含的文件权限在这里并不重要，所以只是使用默认权限。
**
*/
static int findCreateFileMode(
  const char *zPath,              /* Path of file (possibly) being created */
  int flags,                      /* Flags passed as 4th argument to xOpen() */
  mode_t *pMode,                  /* OUT: Permissions to open file with */
  uid_t *pUid,                    /* OUT: uid to set on the file */
  gid_t *pGid                     /* OUT: gid to set on the file */
){
  int rc = SQLITE_OK;             /* Return Code */
  *pMode = 0;
  *pUid = 0;
  *pGid = 0;
  if( flags & (SQLITE_OPEN_WAL|SQLITE_OPEN_MAIN_JOURNAL) ){
    char zDb[MAX_PATHNAME+1];     /* Database file path */
    int nDb;                      /* Number of valid bytes in zDb */
    struct stat sStat;            /* Output of stat() on database file */

    /* zPath is a path to a WAL or journal file. The following block derives
    ** the path to the associated database file from zPath. This block handles
    ** the following naming conventions:
    **
    **   "<path to db>-journal"
    **   "<path to db>-wal"
    **   "<path to db>-journalNN"
    **   "<path to db>-walNN"
    **
    ** where NN is a decimal number. The NN naming schemes are 
    ** used by the test_multiplex.c module.
    **
    ** zPath是WAL或日志文件的路径。 可以从zPath导出该块文件
    ** 到相关数据库文件的路径。块文件的句柄使用下面这些约定的命名方法：
    **
    **“<path to db> -journal”
    **“<path to db> -wal”
    **“<path to db> -journalNN”
    **“<path to db> -walNN”
    **
    **其中NN是十进制数。 NN命名方案由test_multiplex.c模块确定。
    **
    */
    nDb = sqlite3Strlen30(zPath) - 1; 
#ifdef SQLITE_ENABLE_8_3_NAMES
    while( nDb>0 && sqlite3Isalnum(zPath[nDb]) ) nDb--;
    if( nDb==0 || zPath[nDb]!='-' ) return SQLITE_OK;
#else
    while( zPath[nDb]!='-' ){
      assert( nDb>0 );
      assert( zPath[nDb]!='\n' );
      nDb--;
    }
#endif
    memcpy(zDb, zPath, nDb);
    zDb[nDb] = '\0';

    if( 0==osStat(zDb, &sStat) ){
      *pMode = sStat.st_mode & 0777;
      *pUid = sStat.st_uid;
      *pGid = sStat.st_gid;
    }else{
      rc = SQLITE_IOERR_FSTAT;
    }
  }else if( flags & SQLITE_OPEN_DELETEONCLOSE ){
    *pMode = 0600;
  }
  return rc;
}

/*
** Open the file zPath.
** 
** Previously, the SQLite OS layer used three functions in place of this
** one:
**
**     sqlite3OsOpenReadWrite();
**     sqlite3OsOpenReadOnly();
**     sqlite3OsOpenExclusive();
**
** These calls correspond to the following combinations of flags:
**
**     ReadWrite() ->     (READWRITE | CREATE)
**     ReadOnly()  ->     (READONLY) 
**     OpenExclusive() -> (READWRITE | CREATE | EXCLUSIVE)
**
** The old OpenExclusive() accepted a boolean argument - "delFlag". If
** true, the file was configured to be automatically deleted when the
** file handle closed. To achieve the same effect using this new 
** interface, add the DELETEONCLOSE flag to those specified above for 
** OpenExclusive().
**
** 以前，SQLite操作系统层使用这三个函数来代替这一个函数：
**
** 	sqlite3OsOpenReadWrite();
** 	sqlite3OsOpenReadOnly();
** 	sqlite3OsOpenExclusive();
**
** 这些调用和以下的标志组合相对应：
**
** 	ReadWrite() - >(READWRITE | CREATE)
** 	ReadOnly() - >(READONLY)
** 	OpenExclusive() - >(READWRITE | CREATE | EXCLUSIVE)
**
** 旧的OpenExclusive()接受一个布尔参数 - “delFlag”。如果为true，则
** 文件被配置在文件句柄关闭时自动删除。为了在使用新接口时
** 实现相同的效果，请将DELETEONCLOSE标志添加到OpenExclusive()的指定参数处。
**
*/
static int unixOpen(
  sqlite3_vfs *pVfs,           /* The VFS for which this is the xOpen method */
  const char *zPath,           /* Pathname of file to be opened */
  sqlite3_file *pFile,         /* The file descriptor to be filled in */
  int flags,                   /* Input flags to control the opening */
  int *pOutFlags               /* Output flags returned to SQLite core */
){
  unixFile *p = (unixFile *)pFile;
  int fd = -1;                   /* File descriptor returned by open() */
  int openFlags = 0;             /* Flags to pass to open() */
  int eType = flags&0xFFFFFF00;  /* Type of file to open */
  int noLock;                    /* True to omit locking primitives */
  int rc = SQLITE_OK;            /* Function Return Code */
  int ctrlFlags = 0;             /* UNIXFILE_* flags */

  int isExclusive  = (flags & SQLITE_OPEN_EXCLUSIVE);
  int isDelete     = (flags & SQLITE_OPEN_DELETEONCLOSE);
  int isCreate     = (flags & SQLITE_OPEN_CREATE);
  int isReadonly   = (flags & SQLITE_OPEN_READONLY);
  int isReadWrite  = (flags & SQLITE_OPEN_READWRITE);
#if SQLITE_ENABLE_LOCKING_STYLE
  int isAutoProxy  = (flags & SQLITE_OPEN_AUTOPROXY);
#endif
#if defined(__APPLE__) || SQLITE_ENABLE_LOCKING_STYLE
  struct statfs fsInfo;
#endif

  /* If creating a master or main-file journal, this function will open
  ** a file-descriptor on the directory too. The first time unixSync()
  ** is called the directory file descriptor will be fsync()ed and close()d.
  **
  ** 如果创建一个主文件或主文件日志，函数将会在目录上打开
  ** 一个文件描述符。 第一次调用unixSync（）时，目录文件描述符
  ** 将是fsync（）ed和close（）d。
  */
  int syncDir = (isCreate && (
        eType==SQLITE_OPEN_MASTER_JOURNAL 
     || eType==SQLITE_OPEN_MAIN_JOURNAL 
     || eType==SQLITE_OPEN_WAL
  ));

  /* If argument zPath is a NULL pointer, this function is required to open
  ** a temporary file. Use this buffer to store the file name in.
  */
  char zTmpname[MAX_PATHNAME+2];
  const char *zName = zPath;

  /* Check the following statements are true: 
  **
  **   (a) Exactly one of the READWRITE and READONLY flags must be set, and 
  **   (b) if CREATE is set, then READWRITE must also be set, and
  **   (c) if EXCLUSIVE is set, then CREATE must also be set.
  **   (d) if DELETEONCLOSE is set, then CREATE must also be set.
  **
  ** 检查以下语句是否为真：
  **
  **（a）必须设置READWRITE和READONLY标志中的一个
  **（b）如果设置了CREATE，则还必须设置READWRITE
  **（c）如果设置了EXCLUSIVE，那么还必须设置CREATE。
  **（d）如果设置了DELETEONCLOSE，那么还必须设置CREATE。
  **
  */
  assert((isReadonly==0 || isReadWrite==0) && (isReadWrite || isReadonly));
  assert(isCreate==0 || isReadWrite);
  assert(isExclusive==0 || isCreate);
  assert(isDelete==0 || isCreate);

  /* The main DB, main journal, WAL file and master journal are never 
  ** automatically deleted. Nor are they ever temporary files.  */
  assert( (!isDelete && zName) || eType!=SQLITE_OPEN_MAIN_DB );
  assert( (!isDelete && zName) || eType!=SQLITE_OPEN_MAIN_JOURNAL );
  assert( (!isDelete && zName) || eType!=SQLITE_OPEN_MASTER_JOURNAL );
  assert( (!isDelete && zName) || eType!=SQLITE_OPEN_WAL );

  /* Assert that the upper layer has set one of the "file-type" flags. */
  assert( eType==SQLITE_OPEN_MAIN_DB      || eType==SQLITE_OPEN_TEMP_DB 
       || eType==SQLITE_OPEN_MAIN_JOURNAL || eType==SQLITE_OPEN_TEMP_JOURNAL 
       || eType==SQLITE_OPEN_SUBJOURNAL   || eType==SQLITE_OPEN_MASTER_JOURNAL 
       || eType==SQLITE_OPEN_TRANSIENT_DB || eType==SQLITE_OPEN_WAL
  );

  memset(p, 0, sizeof(unixFile));

  if( eType==SQLITE_OPEN_MAIN_DB ){
    UnixUnusedFd *pUnused;
    pUnused = findReusableFd(zName, flags);
    if( pUnused ){
      fd = pUnused->fd;
    }else{
      pUnused = sqlite3_malloc(sizeof(*pUnused));
      if( !pUnused ){
        return SQLITE_NOMEM;
      }
    }
    p->pUnused = pUnused;

    /* Database filenames are double-zero terminated if they are not
    ** URIs with parameters.  Hence, they can always be passed into
    ** sqlite3_uri_parameter(). */
    assert( (flags & SQLITE_OPEN_URI) || zName[strlen(zName)+1]==0 );

  }else if( !zName ){
    /* If zName is NULL, the upper layer is requesting a temp file. */
    assert(isDelete && !syncDir);
    rc = unixGetTempname(MAX_PATHNAME+2, zTmpname);
    if( rc!=SQLITE_OK ){
      return rc;
    }
    zName = zTmpname;

    /* Generated temporary filenames are always double-zero terminated
    ** for use by sqlite3_uri_parameter(). */
    assert( zName[strlen(zName)+1]==0 );
  }

  /* Determine the value of the flags parameter passed to POSIX function
  ** open(). These must be calculated even if open() is not called, as
  ** they may be stored as part of the file handle and used by the 
  ** 'conch file' locking functions later on.  
  **
  ** 确定传递给POSIX函数open()参数flags的值。 这些必须计算，
  ** 即使open()没有被调用，因为它们可能被存储为文件句柄的一部分，
  ** 并由“conch文件”锁定功能以后使用。
  */
  if( isReadonly )  openFlags |= O_RDONLY;
  if( isReadWrite ) openFlags |= O_RDWR;
  if( isCreate )    openFlags |= O_CREAT;
  if( isExclusive ) openFlags |= (O_EXCL|O_NOFOLLOW);
  openFlags |= (O_LARGEFILE|O_BINARY);

  if( fd<0 ){
    mode_t openMode;              /* Permissions to create file with */
    uid_t uid;                    /* Userid for the file */
    gid_t gid;                    /* Groupid for the file */
    rc = findCreateFileMode(zName, flags, &openMode, &uid, &gid);
    if( rc!=SQLITE_OK ){
      assert( !p->pUnused );
      assert( eType==SQLITE_OPEN_WAL || eType==SQLITE_OPEN_MAIN_JOURNAL );
      return rc;
    }
    fd = robust_open(zName, openFlags, openMode);
    OSTRACE(("OPENX   %-3d %s 0%o\n", fd, zName, openFlags));
    if( fd<0 && errno!=EISDIR && isReadWrite && !isExclusive ){
      /* Failed to open the file for read/write access. Try read-only. */
      flags &= ~(SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE);
      openFlags &= ~(O_RDWR|O_CREAT);
      flags |= SQLITE_OPEN_READONLY;
      openFlags |= O_RDONLY;
      isReadonly = 1;
      fd = robust_open(zName, openFlags, openMode);
    }
    if( fd<0 ){
      rc = unixLogError(SQLITE_CANTOPEN_BKPT, "open", zName);
      goto open_finished;
    }

    /* If this process is running as root and if creating a new rollback
    ** journal or WAL file, set the ownership of the journal or WAL to be
    ** the same as the original database.
    */
    if( flags & (SQLITE_OPEN_WAL|SQLITE_OPEN_MAIN_JOURNAL) ){
      osFchown(fd, uid, gid);
    }
  }
  assert( fd>=0 );
  if( pOutFlags ){
    *pOutFlags = flags;
  }

  if( p->pUnused ){
    p->pUnused->fd = fd;
    p->pUnused->flags = flags;
  }

  if( isDelete ){
#if OS_VXWORKS
    zPath = zName;
#else
    osUnlink(zName);
#endif
  }
#if SQLITE_ENABLE_LOCKING_STYLE
  else{
    p->openFlags = openFlags;
  }
#endif

  noLock = eType!=SQLITE_OPEN_MAIN_DB;

  
#if defined(__APPLE__) || SQLITE_ENABLE_LOCKING_STYLE
  if( fstatfs(fd, &fsInfo) == -1 ){
    ((unixFile*)pFile)->lastErrno = errno;
    robust_close(p, fd, __LINE__);
    return SQLITE_IOERR_ACCESS;
  }
  if (0 == strncmp("msdos", fsInfo.f_fstypename, 5)) {
    ((unixFile*)pFile)->fsFlags |= SQLITE_FSFLAGS_IS_MSDOS;
  }
#endif

  /* Set up appropriate ctrlFlags */
  if( isDelete )                ctrlFlags |= UNIXFILE_DELETE;
  if( isReadonly )              ctrlFlags |= UNIXFILE_RDONLY;
  if( noLock )                  ctrlFlags |= UNIXFILE_NOLOCK;
  if( syncDir )                 ctrlFlags |= UNIXFILE_DIRSYNC;
  if( flags & SQLITE_OPEN_URI ) ctrlFlags |= UNIXFILE_URI;

#if SQLITE_ENABLE_LOCKING_STYLE
#if SQLITE_PREFER_PROXY_LOCKING
  isAutoProxy = 1;
#endif
  if( isAutoProxy && (zPath!=NULL) && (!noLock) && pVfs->xOpen ){
    char *envforce = getenv("SQLITE_FORCE_PROXY_LOCKING");
    int useProxy = 0;

    /* SQLITE_FORCE_PROXY_LOCKING==1 means force always use proxy, 0 means 
    ** never use proxy, NULL means use proxy for non-local files only.  */
    if( envforce!=NULL ){
      useProxy = atoi(envforce)>0;
    }else{
      if( statfs(zPath, &fsInfo) == -1 ){
        /* In theory, the close(fd) call is sub-optimal. If the file opened
        ** with fd is a database file, and there are other connections open
        ** on that file that are currently holding advisory locks on it,
        ** then the call to close() will cancel those locks. In practice,
        ** we're assuming that statfs() doesn't fail very often. At least
        ** not while other file descriptors opened by the same process on
        ** the same file are working.  
        ** 理论上，close（fd）调用是次优的。如果使用fd打开的是数据库文件，
        ** 并且在该打开文件上其他连接持有咨询锁，则调用close()取消这些锁。
        ** 在实践中，我们假设statfs()不会经常失败。至少不会是同一文件上的
        ** 同一进程打开的其他文件描述符时失败.
        ** 
        */
        p->lastErrno = errno;
        robust_close(p, fd, __LINE__);
        rc = SQLITE_IOERR_ACCESS;
        goto open_finished;
      }
      useProxy = !(fsInfo.f_flags&MNT_LOCAL);
    }
    if( useProxy ){
      rc = fillInUnixFile(pVfs, fd, pFile, zPath, ctrlFlags);
      if( rc==SQLITE_OK ){
        rc = proxyTransformUnixFile((unixFile*)pFile, ":auto:");
        if( rc!=SQLITE_OK ){
          /* Use unixClose to clean up the resources added in fillInUnixFile 
          ** and clear all the structure's references.  Specifically, 
          ** pFile->pMethods will be NULL so sqlite3OsClose will be a no-op 
          */
          unixClose(pFile);
          return rc;
        }
      }
      goto open_finished;
    }
  }
#endif
  
  rc = fillInUnixFile(pVfs, fd, pFile, zPath, ctrlFlags);

open_finished:
  if( rc!=SQLITE_OK ){
    sqlite3_free(p->pUnused);
  }
  return rc;
}


/*
** Delete the file at zPath. If the dirSync argument is true, fsync()
** the directory after deleting the file.
*/
static int unixDelete(
  sqlite3_vfs *NotUsed,     /* VFS containing this as the xDelete method */
  const char *zPath,        /* Name of file to be deleted */
  int dirSync               /* If true, fsync() directory after deleting file */
){
  int rc = SQLITE_OK;
  UNUSED_PARAMETER(NotUsed);
  SimulateIOError(return SQLITE_IOERR_DELETE);
  if( osUnlink(zPath)==(-1) && errno!=ENOENT ){
    return unixLogError(SQLITE_IOERR_DELETE, "unlink", zPath);
  }
#ifndef SQLITE_DISABLE_DIRSYNC
  if( (dirSync & 1)!=0 ){
    int fd;
    rc = osOpenDirectory(zPath, &fd);
    if( rc==SQLITE_OK ){
#if OS_VXWORKS
      if( fsync(fd)==-1 )
#else
      if( fsync(fd) )
#endif
      {
        rc = unixLogError(SQLITE_IOERR_DIR_FSYNC, "fsync", zPath);
      }
      robust_close(0, fd, __LINE__);
    }else if( rc==SQLITE_CANTOPEN ){
      rc = SQLITE_OK;
    }
  }
#endif
  return rc;
}

/*
** Test the existance of or access permissions of file zPath. The
** test performed depends on the value of flags:
**
**     SQLITE_ACCESS_EXISTS: Return 1 if the file exists
**     SQLITE_ACCESS_READWRITE: Return 1 if the file is read and writable.
**     SQLITE_ACCESS_READONLY: Return 1 if the file is readable.
**
** Otherwise return 0.
*/
static int unixAccess(
  sqlite3_vfs *NotUsed,   /* The VFS containing this xAccess method */
  const char *zPath,      /* Path of the file to examine */
  int flags,              /* What do we want to learn about the zPath file? */
  int *pResOut            /* Write result boolean here */
){
  int amode = 0;
  UNUSED_PARAMETER(NotUsed);
  SimulateIOError( return SQLITE_IOERR_ACCESS; );
  switch( flags ){
    case SQLITE_ACCESS_EXISTS:
      amode = F_OK;
      break;
    case SQLITE_ACCESS_READWRITE:
      amode = W_OK|R_OK;
      break;
    case SQLITE_ACCESS_READ:
      amode = R_OK;
      break;

    default:
      assert(!"Invalid flags argument");
  }
  *pResOut = (osAccess(zPath, amode)==0);
  if( flags==SQLITE_ACCESS_EXISTS && *pResOut ){
    struct stat buf;
    if( 0==osStat(zPath, &buf) && buf.st_size==0 ){
      *pResOut = 0;
    }
  }
  return SQLITE_OK;
}


/*
** Turn a relative pathname into a full pathname. The relative path
** is stored as a nul-terminated string in the buffer pointed to by
** zPath. 
**
** zOut points to a buffer of at least sqlite3_vfs.mxPathname bytes 
** (in this case, MAX_PATHNAME bytes). The full-path is written to
** this buffer before returning.
**
** 将相对路径名转换为完整路径名。 相对路径作为一个nul终止字符串
** 存储在zPath指向的缓冲区中。
** zOut指向至少为sqlite3_vfs.mxPathname字节（在本例中为MAX_PATHNAME个字节）的缓冲区。 
** 在返回之前，全路径被写入此缓冲区。
**
*/
static int unixFullPathname(
  sqlite3_vfs *pVfs,            /* Pointer to vfs object */
  const char *zPath,            /* Possibly relative input path */
  int nOut,                     /* Size of output buffer in bytes */
  char *zOut                    /* Output buffer */
){

  /* It's odd to simulate an io-error here, but really this is just
  ** using the io-error infrastructure to test that SQLite handles this
  ** function failing. This function could fail if, for example, the
  ** current working directory has been unlinked.
  */
  SimulateIOError( return SQLITE_ERROR );

  assert( pVfs->mxPathname==MAX_PATHNAME );
  UNUSED_PARAMETER(pVfs);

  zOut[nOut-1] = '\0';
  if( zPath[0]=='/' ){
    sqlite3_snprintf(nOut, zOut, "%s", zPath);
  }else{
    int nCwd;
    if( osGetcwd(zOut, nOut-1)==0 ){
      return unixLogError(SQLITE_CANTOPEN_BKPT, "getcwd", zPath);
    }
    nCwd = (int)strlen(zOut);
    sqlite3_snprintf(nOut-nCwd, &zOut[nCwd], "/%s", zPath);
  }
  return SQLITE_OK;
}


#ifndef SQLITE_OMIT_LOAD_EXTENSION
/*
** Interfaces for opening a shared library, finding entry points
** within the shared library, and closing the shared library.
** 
** 用于打开共享库，在共享库中查找入口点以及关闭共享库的接口。
**
*/
#include <dlfcn.h>
static void *unixDlOpen(sqlite3_vfs *NotUsed, const char *zFilename){
  UNUSED_PARAMETER(NotUsed);
  return dlopen(zFilename, RTLD_NOW | RTLD_GLOBAL);
}

/*
** SQLite calls this function immediately after a call to unixDlSym() or
** unixDlOpen() fails (returns a null pointer). If a more detailed error
** message is available, it is written to zBufOut. If no error message
** is available, zBufOut is left unmodified and SQLite uses a default
** error message.
**
** SQLite在调用unixDlSym()或unixDlOpen()失败（返回一个空指针）后立即调用此函数。
** 如果有更详细的错误消息可用，将它写入zBufOut。 如果没有可用的错误消息，
** zBufOut保持不变，SQLite使用默认错误消息。
*/
static void unixDlError(sqlite3_vfs *NotUsed, int nBuf, char *zBufOut){
  const char *zErr;
  UNUSED_PARAMETER(NotUsed);
  unixEnterMutex();
  zErr = dlerror();
  if( zErr ){
    sqlite3_snprintf(nBuf, zBufOut, "%s", zErr);
  }
  unixLeaveMutex();
}
static void (*unixDlSym(sqlite3_vfs *NotUsed, void *p, const char*zSym))(void){
  /* 
  ** GCC with -pedantic-errors says that C90 does not allow a void* to be
  ** cast into a pointer to a function.  And yet the library dlsym() routine
  ** returns a void* which is really a pointer to a function.  So how do we
  ** use dlsym() with -pedantic-errors?
  **
  ** Variable x below is defined to be a pointer to a function taking
  ** parameters void* and const char* and returning a pointer to a function.
  ** We initialize x by assigning it a pointer to the dlsym() function.
  ** (That assignment requires a cast.)  Then we call the function that
  ** x points to.  
  **
  ** This work-around is unlikely to work correctly on any system where
  ** you really cannot cast a function pointer into void*.  But then, on the
  ** other hand, dlsym() will not work on such a system either, so we have
  ** not really lost anything.
  **
  ** GCC with -pedantic-errors指明C90不允许将void *转换为指向函数的指针。
  ** 然而库dlsym()例程返回一个void *，这实际上是一个指向一个函数的指针。
  ** 那么我们如何使用dlsym()和-pedantic-errors呢?
  **
  ** 下面的变量x被定义为一个指向一个函数的指针，该函数接受参数void *和const char *
  ** 并返回一个指向函数的指针。 我们通过为dlsym()函数分配一个指针来初始化x。 
  **（这个赋值需要一个转换。）然后我们调用x指向的函数。
  **
  ** 这种解决方法不可能在任何系统上正确工作，你确实不能将函数指针转换为void*。 
  ** 但在另一方面，dlsym()也不会在这样的系统上工作，所以我们实际上没有失去任何东西。
  **
  */
  void (*(*x)(void*,const char*))(void);
  UNUSED_PARAMETER(NotUsed);
  x = (void(*(*)(void*,const char*))(void))dlsym;
  return (*x)(p, zSym);
}
static void unixDlClose(sqlite3_vfs *NotUsed, void *pHandle){
  UNUSED_PARAMETER(NotUsed);
  dlclose(pHandle);
}
#else /* if SQLITE_OMIT_LOAD_EXTENSION is defined: */
  #define unixDlOpen  0
  #define unixDlError 0
  #define unixDlSym   0
  #define unixDlClose 0
#endif

/*
** Write nBuf bytes of random data to the supplied buffer zBuf.
*/
static int unixRandomness(sqlite3_vfs *NotUsed, int nBuf, char *zBuf){
  UNUSED_PARAMETER(NotUsed);
  assert((size_t)nBuf>=(sizeof(time_t)+sizeof(int)));

  /* We have to initialize zBuf to prevent valgrind from reporting
  ** errors.  The reports issued by valgrind are incorrect - we would
  ** prefer that the randomness be increased by making use of the
  ** uninitialized space in zBuf - but valgrind errors tend to worry
  ** some users.  Rather than argue, it seems easier just to initialize
  ** the whole array and silence valgrind, even if that means less randomness
  ** in the random seed.
  **
  ** When testing, initializing zBuf[] to zero is all we do.  That means
  ** that we always use the same random number sequence.  This makes the
  ** tests repeatable.
  */
  memset(zBuf, 0, nBuf);
#if !defined(SQLITE_TEST)
  {
    int pid, fd, got;
    fd = robust_open("/dev/urandom", O_RDONLY, 0);
    if( fd<0 ){
      time_t t;
      time(&t);
      memcpy(zBuf, &t, sizeof(t));
      pid = getpid();
      memcpy(&zBuf[sizeof(t)], &pid, sizeof(pid));
      assert( sizeof(t)+sizeof(pid)<=(size_t)nBuf );
      nBuf = sizeof(t) + sizeof(pid);
    }else{
      do{ got = osRead(fd, zBuf, nBuf); }while( got<0 && errno==EINTR );
      robust_close(0, fd, __LINE__);
    }
  }
#endif
  return nBuf;
}


/*
** Sleep for a little while.  Return the amount of time slept.
** The argument is the number of microseconds we want to sleep.
** The return value is the number of microseconds of sleep actually
** requested from the underlying operating system, a number which
** might be greater than or equal to the argument, but not less
** than the argument.
**
** 休眠一段时间。 返回休眠的时间量。 参数是我们想要休眠的微秒数。 
** 返回值是从底层操作系统实际请求的休眠的微秒数，该数字可能大于
** 或等于参数，但不小于参数。
*/
static int unixSleep(sqlite3_vfs *NotUsed, int microseconds){
#if OS_VXWORKS
  struct timespec sp;

  sp.tv_sec = microseconds / 1000000;
  sp.tv_nsec = (microseconds % 1000000) * 1000;
  nanosleep(&sp, NULL);
  UNUSED_PARAMETER(NotUsed);
  return microseconds;
#elif defined(HAVE_USLEEP) && HAVE_USLEEP
  usleep(microseconds);
  UNUSED_PARAMETER(NotUsed);
  return microseconds;
#else
  int seconds = (microseconds+999999)/1000000;
  sleep(seconds);
  UNUSED_PARAMETER(NotUsed);
  return seconds*1000000;
#endif
}

/*
** The following variable, if set to a non-zero value, is interpreted as
** the number of seconds since 1970 and is used to set the result of
** sqlite3OsCurrentTime() during testing.
*/
#ifdef SQLITE_TEST
int sqlite3_current_time = 0;  /* Fake system time in seconds since 1970. */
#endif

/*
** Find the current time (in Universal Coordinated Time).  Write into *piNow
** the current time and date as a Julian Day number times 86_400_000.  In
** other words, write into *piNow the number of milliseconds since the Julian
** epoch of noon in Greenwich on November 24, 4714 B.C according to the
** proleptic Gregorian calendar.
**
** On success, return SQLITE_OK.  Return SQLITE_ERROR if the time and date 
** cannot be found.
**
** 查找当前时间（在协调时间内）。 写入* piNow当前时间和日期为朱利安日数
** 乘以86_400_000。 换句话说，写入* piNow自从中午的朱利安时代以来的毫秒数，
** 格林威治在4714年11月24日，根据proleptic格里历。
**
** 成功后，返回SQLITE_OK。 如果无法找到时间和日期，则返回SQLITE_ERROR。
*/
static int unixCurrentTimeInt64(sqlite3_vfs *NotUsed, sqlite3_int64 *piNow){
  static const sqlite3_int64 unixEpoch = 24405875*(sqlite3_int64)8640000;
  int rc = SQLITE_OK;
#if defined(NO_GETTOD)
  time_t t;
  time(&t);
  *piNow = ((sqlite3_int64)t)*1000 + unixEpoch;
#elif OS_VXWORKS
  struct timespec sNow;
  clock_gettime(CLOCK_REALTIME, &sNow);
  *piNow = unixEpoch + 1000*(sqlite3_int64)sNow.tv_sec + sNow.tv_nsec/1000000;
#else
  struct timeval sNow;
  if( gettimeofday(&sNow, 0)==0 ){
    *piNow = unixEpoch + 1000*(sqlite3_int64)sNow.tv_sec + sNow.tv_usec/1000;
  }else{
    rc = SQLITE_ERROR;
  }
#endif

#ifdef SQLITE_TEST
  if( sqlite3_current_time ){
    *piNow = 1000*(sqlite3_int64)sqlite3_current_time + unixEpoch;
  }
#endif
  UNUSED_PARAMETER(NotUsed);
  return rc;
}

/*
** Find the current time (in Universal Coordinated Time).  Write the
** current time and date as a Julian Day number into *prNow and
** return 0.  Return 1 if the time and date cannot be found.
*/
static int unixCurrentTime(sqlite3_vfs *NotUsed, double *prNow){
  sqlite3_int64 i = 0;
  int rc;
  UNUSED_PARAMETER(NotUsed);
  rc = unixCurrentTimeInt64(0, &i);
  *prNow = i/86400000.0;
  return rc;
}

/*
** We added the xGetLastError() method with the intention of providing
** better low-level error messages when operating-system problems come up
** during SQLite operation.  But so far, none of that has been implemented
** in the core.  So this routine is never called.  For now, it is merely
** a place-holder.
**
** 我们添加了xGetLastError（）方法，目的是在SQLite操作出现操作系统问题时
** 提供更好的低级错误消息。 但到目前为止，这些都没有在核心实施。 
** 所以这个例程不会被调用。 现在只是一个占位符。
**
*/
static int unixGetLastError(sqlite3_vfs *NotUsed, int NotUsed2, char *NotUsed3){
  UNUSED_PARAMETER(NotUsed);
  UNUSED_PARAMETER(NotUsed2);
  UNUSED_PARAMETER(NotUsed3);
  return 0;
}


/*
************************ End of sqlite3_vfs methods ***************************
******************************************************************************/

/******************************************************************************
************************** Begin Proxy Locking ********************************
**
** Proxy locking is a "uber-locking-method" in this sense:  It uses the
** other locking methods on secondary lock files.  Proxy locking is a
** meta-layer over top of the primitive locking implemented above.  For
** this reason, the division that implements of proxy locking is deferred
** until late in the file (here) after all of the other I/O methods have
** been defined - so that the primitive locking methods are available
** as services to help with the implementation of proxy locking.
**
** 在某种意义上，代理锁是一种“uber锁定方法”：它对辅助锁文件
** 使用其他锁定方法。 代理锁定是在上面实现的原语锁定之上的元层。 
** 因此，在所有其他I / O方法被定义之后，实现代理锁定的分割被推迟到
** 后面（这里） - 以使得原语锁定方法可用作服务实现代理锁定。
**
****
**
** The default locking schemes in SQLite use byte-range locks on the
** database file to coordinate safe, concurrent access by multiple readers
** and writers [http://sqlite.org/lockingv3.html].  The five file locking
** states (UNLOCKED, PENDING, SHARED, RESERVED, EXCLUSIVE) are implemented
** as POSIX read & write locks over fixed set of locations (via fsctl),
** on AFP and SMB only exclusive byte-range locks are available via fsctl
** with _IOWR('z', 23, struct ByteRangeLockPB2) to track the same 5 states.
** To simulate a F_RDLCK on the shared range, on AFP a randomly selected
** address in the shared range is taken for a SHARED lock, the entire
** shared range is taken for an EXCLUSIVE lock):
**
** SQLite中的默认锁定方案使用数据库文件上的字节范围锁定来协调安全的，
** 由多个读取器和写入器的并发访问[http://sqlite.org/lockingv3.html]。 五个文件锁定状态
**（UNLOCKED，PENDING，SHARED，RESERVED，EXCLUSIVE）被实现为固定位置集合上的POSIX
** 读写锁定（通过fsctl），在AFP和SMB上，只有专用的字节范围锁定通过fsctl和_IOWR 
**（'z'，23，struct ByteRangeLockPB2）来跟踪相同的5个状态。 为了在共享范围上模拟F_RDLCK，
** 在AFP上，共享范围中随机选择的地址用于SHARED锁，整个共享范围用于EXCLUSIVE锁）：
**
**      PENDING_BYTE        0x40000000
**      RESERVED_BYTE       0x40000001
**      SHARED_RANGE        0x40000002 -> 0x40000200
**
** This works well on the local file system, but shows a nearly 100x
** slowdown in read performance on AFP because the AFP client disables
** the read cache when byte-range locks are present.  Enabling the read
** cache exposes a cache coherency problem that is present on all OS X
** supported network file systems.  NFS and AFP both observe the
** close-to-open semantics for ensuring cache coherency
** [http://nfs.sourceforge.net/#faq_a8], which does not effectively
** address the requirements for concurrent database access by multiple
** readers and writers
** [http://www.nabble.com/SQLite-on-NFS-cache-coherency-td15655701.html].
**
** To address the performance and cache coherency issues, proxy file locking
** changes the way database access is controlled by limiting access to a
** single host at a time and moving file locks off of the database file
** and onto a proxy file on the local file system.  
**
** PENDING_BYTE 0x40000000
** RESERVED_BYTE 0x40000001
** SHARED_RANGE 0x40000002 - > 0x40000200
**
** 这在本地文件系统上工作效果很好，但是在AFP上的读取性能下降了近100倍，
** 因为AFP客户端在存在字节范围锁时禁用读取缓存。启用读取缓存会暴露所有
** OS X支持的网络文件系统上存在的缓存一致性问题。 NFS和AFP都遵守接近开放的
** 语义来确保缓存一致性[http://nfs.sourceforge.net/#faq_a8]，这没有有效地满足多个读取器
** 和写入器并发数据库访问的要求[http： //www.nabble.com/SQLite-on-NFS-cache-coherency-td15655701.html]。
**
** 为了解决性能和缓存一致性问题，代理文件锁定改变了数据库访问的方式，
** 通过限制对单个主机的访问以及将文件锁从数据库文件移动到本地文件系统上的
** 代理文件。
**
**
** Using proxy locks
** -----------------
**
** C APIs
**
**  sqlite3_file_control(db, dbname, SQLITE_SET_LOCKPROXYFILE,
**                       <proxy_path> | ":auto:");
**  sqlite3_file_control(db, dbname, SQLITE_GET_LOCKPROXYFILE, &<proxy_path>);
**
**
** SQL pragmas
**
**  PRAGMA [database.]lock_proxy_file=<proxy_path> | :auto:
**  PRAGMA [database.]lock_proxy_file
**
** Specifying ":auto:" means that if there is a conch file with a matching
** host ID in it, the proxy path in the conch file will be used, otherwise
** a proxy path based on the user's temp dir
** (via confstr(_CS_DARWIN_USER_TEMP_DIR,...)) will be used and the
** actual proxy file name is generated from the name and path of the
** database file.  For example:
**
**       For database path "/Users/me/foo.db" 
**       The lock path will be "<tmpdir>/sqliteplocks/_Users_me_foo.db:auto:")
**
** 使用代理锁
** -----------------
**
** C API
**
** sqlite3_file_control（db，dbname，SQLITE_SET_LOCKPROXYFILE，
** <proxy_path> | “：auto：”）;
** sqlite3_file_control（db，dbname，SQLITE_GET_LOCKPROXYFILE，＆<proxy_path>）;
**
**
** SQL编译指示
**
** PRAGMA [database。] lock_proxy_file = <proxy_path> | ：auto：
** PRAGMA [database。] lock_proxy_file
**
** 指定“：auto：”意味着如果有一个具有匹配的主机ID的文件，则将使用conch文件
** 中的代理路径，否则将使用基于用户的temp dir的代理路径（通过confstr（_CS_DARWIN_USER_TEMP_DIR ，...）），
** 并且实际的代理文件名是从数据库文件的名称和路径生成的。 例如：
**
** 对于数据库路径“/Users/me/foo.db”
** 锁定路径将为“<tmpdir> /sqliteplocks/_Users_me_foo.db:auto：”）
**
** Once a lock proxy is configured for a database connection, it can not
** be removed, however it may be switched to a different proxy path via
** the above APIs (assuming the conch file is not being held by another
** connection or process). 
**
** 一旦锁代理被配置用于数据库连接，则它不能被移除，然而它可以经由上述API
** 被切换到不同的代理路径（假设该conch文件没有被另一个连接或进程使用）。
**
**
** How proxy locking works
** -----------------------
**
** Proxy file locking relies primarily on two new supporting files: 
**
**   *  conch file to limit access to the database file to a single host
**      at a time
**
**   *  proxy file to act as a proxy for the advisory locks normally
**      taken on the database
**
** The conch file - to use a proxy file, sqlite must first "hold the conch"
** by taking an sqlite-style shared lock on the conch file, reading the
** contents and comparing the host's unique host ID (see below) and lock
** proxy path against the values stored in the conch.  The conch file is
** stored in the same directory as the database file and the file name
** is patterned after the database file name as ".<databasename>-conch".
** If the conch file does not exist, or it's contents do not match the
** host ID and/or proxy path, then the lock is escalated to an exclusive
** lock and the conch file contents is updated with the host ID and proxy
** path and the lock is downgraded to a shared lock again.  If the conch
** is held by another process (with a shared lock), the exclusive lock
** will fail and SQLITE_BUSY is returned.
**
** 代理锁定的工作原理
** -----------------------
**
** 代理文件锁定主要依赖于两个新的支持文件：
**
** * conch文件，以限制对单个主机的数据库文件的访问
**
** * 代理文件用作通常在数据库上执行的咨询锁的代理
**
** conch文件 -要使用代理文件，sqlite必须首先通过对conch文件采取sqlite共享锁来保持conch，
** 读取内容并比较主机的唯一主机ID（请参见下文）和锁代理路径对存储的conch文件值。
** conch文件存储在与数据库文件相同的目录中，并且在数据库文件名后面的文件名被
** 图案化为“<databasename> -conch”。如果conch文件不存在，或者其内容与主机ID和/或代理
** 路径不匹配，则锁升级为独占锁，并且conch文件内容用主机ID和代理路径更新，并且锁是
** 降级到共享锁。如果conch文件由另一个进程（具有共享锁）持有，则排它锁将失败，
** 并返回SQLITE_BUSY。
**
** The proxy file - a single-byte file used for all advisory file locks
** normally taken on the database file.   This allows for safe sharing
** of the database file for multiple readers and writers on the same
** host (the conch ensures that they all use the same local lock file).
**
** Requesting the lock proxy does not immediately take the conch, it is
** only taken when the first request to lock database file is made.  
** This matches the semantics of the traditional locking behavior, where
** opening a connection to a database file does not take a lock on it.
** The shared lock and an open file descriptor are maintained until 
** the connection to the database is closed. 
**
** The proxy file and the lock file are never deleted so they only need
** to be created the first time they are used.
**
** 代理文件 - 用于通常在数据库文件上采用的所有建议文件锁的单字节文件。 
** 这允许在同一主机上的多个读取器和写入器的数据库文件的安全共享
**（该conch确保它们都使用相同的本地锁文件）。
**
** 请求锁代理不立即采取海螺，它只有在第一次请求锁定数据库文件时才采取。 
** 这匹配传统锁定行为的语义，其中打开与数据库文件的连接不会对其进行锁定。 
** 将保持共享锁和打开的文件描述符，直到与数据库的连接关闭。
**
** 代理文件和锁定文件从不删除，因此只需要在首次使用它们时创建它们。
**
** Configuration options
** ---------------------
**
**  SQLITE_PREFER_PROXY_LOCKING
**
**       Database files accessed on non-local file systems are
**       automatically configured for proxy locking, lock files are
**       named automatically using the same logic as
**       PRAGMA lock_proxy_file=":auto:"
**    
**  SQLITE_PROXY_DEBUG
**
**       Enables the logging of error messages during host id file
**       retrieval and creation
**
**  LOCKPROXYDIR
**
**       Overrides the default directory used for lock proxy files that
**       are named automatically via the ":auto:" setting
**
**  SQLITE_DEFAULT_PROXYDIR_PERMISSIONS
**
**       Permissions to use when creating a directory for storing the
**       lock proxy files, only used when LOCKPROXYDIR is not set.
**    
** 配置选项
** ---------------------
**
** SQLITE_PREFER_PROXY_LOCKING
**
** 在非本地文件系统上访问的数据库文件会自动配置为代理锁定，
** 锁定文件将使用相同的逻辑自动命名
** 				PRAGMA lock_proxy_file =“：auto：”
**
** SQLITE_PROXY_DEBUG
**
** 在主机ID文件检索和创建期间启用错误消息的日志记录
**
** LOCKPROXYDIR
**
** 覆盖用于通过“：auto：”设置自动命名的锁定代理文件的默认目录
**
** SQLITE_DEFAULT_PROXYDIR_PERMISSIONS
**
** 创建用于存储锁定代理文件的目录时使用的权限，仅在未设置LOCKPROXYDIR时使用。
**    
** As mentioned above, when compiled with SQLITE_PREFER_PROXY_LOCKING,
** setting the environment variable SQLITE_FORCE_PROXY_LOCKING to 1 will
** force proxy locking to be used for every database file opened, and 0
** will force automatic proxy locking to be disabled for all database
** files (explicity calling the SQLITE_SET_LOCKPROXYFILE pragma or
** sqlite_file_control API is not affected by SQLITE_FORCE_PROXY_LOCKING).
** 
** 如上所述，当使用SQLITE_PREFER_PROXY_LOCKING编译时，将环境变量SQLITE_FORCE_PROXY_LOCKING
** 设置为1将强制代理锁定用于每个打开的数据库文件，0将强制为所有数据库文件
** 禁用自动代理锁定（显式调用SQLITE_SET_LOCKPROXYFILE伪指令或 sqlite_file_control API不受
** SQLITE_FORCE_PROXY_LOCKING影响）。
**
*/

/*
** Proxy locking is only available on MacOSX 代理锁定仅在MacOSX上可用
*/
#if defined(__APPLE__) && SQLITE_ENABLE_LOCKING_STYLE

/*
** The proxyLockingContext has the path and file structures for the remote 
** and local proxy files in it
**
** proxyLockingContext具有远程和本地代理文件两种路径和文件结构
**
*/
typedef struct proxyLockingContext proxyLockingContext;
struct proxyLockingContext {
  unixFile *conchFile;         /* Open conch file */
  char *conchFilePath;         /* Name of the conch file */
  unixFile *lockProxy;         /* Open proxy lock file */
  char *lockProxyPath;         /* Name of the proxy lock file */
  char *dbPath;                /* Name of the open file */
  int conchHeld;               /* 1 if the conch is held, -1 if lockless */
  void *oldLockingContext;     /* Original lockingcontext to restore on close */
  sqlite3_io_methods const *pOldMethod;     /* Original I/O methods for close */
};

/* 
** The proxy lock file path for the database at dbPath is written into lPath, 
** which must point to valid, writable memory large enough for a maxLen length
** file path. 
** 
** dbPath处的数据库的代理锁文件路径写入lPath，该路径必须指向足够大的
** maxLen长度文件路径的有效可写内存。
**
*/
static int proxyGetLockPath(const char *dbPath, char *lPath, size_t maxLen){
  int len;
  int dbLen;
  int i;

#ifdef LOCKPROXYDIR
  len = strlcpy(lPath, LOCKPROXYDIR, maxLen);
#else
# ifdef _CS_DARWIN_USER_TEMP_DIR
  {
    if( !confstr(_CS_DARWIN_USER_TEMP_DIR, lPath, maxLen) ){
      OSTRACE(("GETLOCKPATH  failed %s errno=%d pid=%d\n",
               lPath, errno, getpid()));
      return SQLITE_IOERR_LOCK;
    }
    len = strlcat(lPath, "sqliteplocks", maxLen);    
  }
# else
  len = strlcpy(lPath, "/tmp/", maxLen);
# endif
#endif

  if( lPath[len-1]!='/' ){
    len = strlcat(lPath, "/", maxLen);
  }
  
  /* transform the db path to a unique cache name */
  dbLen = (int)strlen(dbPath);
  for( i=0; i<dbLen && (i+len+7)<(int)maxLen; i++){
    char c = dbPath[i];
    lPath[i+len] = (c=='/')?'_':c;
  }
  lPath[i+len]='\0';
  strlcat(lPath, ":auto:", maxLen);
  OSTRACE(("GETLOCKPATH  proxy lock path=%s pid=%d\n", lPath, getpid()));
  return SQLITE_OK;
}

/* 
 ** Creates the lock file and any missing directories in lockPath
 */
static int proxyCreateLockPath(const char *lockPath){
  int i, len;
  char buf[MAXPATHLEN];
  int start = 0;
  
  assert(lockPath!=NULL);
  /* try to create all the intermediate directories */
  len = (int)strlen(lockPath);
  buf[0] = lockPath[0];
  for( i=1; i<len; i++ ){
    if( lockPath[i] == '/' && (i - start > 0) ){
      /* only mkdir if leaf dir != "." or "/" or ".." */
      if( i-start>2 || (i-start==1 && buf[start] != '.' && buf[start] != '/') 
         || (i-start==2 && buf[start] != '.' && buf[start+1] != '.') ){
        buf[i]='\0';
        if( osMkdir(buf, SQLITE_DEFAULT_PROXYDIR_PERMISSIONS) ){
          int err=errno;
          if( err!=EEXIST ) {
            OSTRACE(("CREATELOCKPATH  FAILED creating %s, "
                     "'%s' proxy lock path=%s pid=%d\n",
                     buf, strerror(err), lockPath, getpid()));
            return err;
          }
        }
      }
      start=i+1;
    }
    buf[i] = lockPath[i];
  }
  OSTRACE(("CREATELOCKPATH  proxy lock path=%s pid=%d\n", lockPath, getpid()));
  return 0;
}

/*
** Create a new VFS file descriptor (stored in memory obtained from
** sqlite3_malloc) and open the file named "path" in the file descriptor.
**
** The caller is responsible not only for closing the file descriptor
** but also for freeing the memory associated with the file descriptor.
*/
static int proxyCreateUnixFile(
    const char *path,        /* path for the new unixFile */
    unixFile **ppFile,       /* unixFile created and returned by ref */
    int islockfile           /* if non zero missing dirs will be created */
) {
  int fd = -1;
  unixFile *pNew;
  int rc = SQLITE_OK;
  int openFlags = O_RDWR | O_CREAT;
  sqlite3_vfs dummyVfs;
  int terrno = 0;
  UnixUnusedFd *pUnused = NULL;

  /* 1. first try to open/create the file
  ** 2. if that fails, and this is a lock file (not-conch), try creating
  ** the parent directories and then try again.
  ** 3. if that fails, try to open the file read-only
  ** otherwise return BUSY (if lock file) or CANTOPEN for the conch file
  ** 
  ** 1.首先尝试打开/创建文件
  ** 2.如果失败，并且这是一个锁文件（非conch），请尝试创建父目录，然后重试。
  ** 3.如果失败，尝试打开文件只读，否则返回BUSY（如果锁定文件）或CANTOPEN为conch文件
  **
  */
  pUnused = findReusableFd(path, openFlags);
  if( pUnused ){
    fd = pUnused->fd;
  }else{
    pUnused = sqlite3_malloc(sizeof(*pUnused));
    if( !pUnused ){
      return SQLITE_NOMEM;
    }
  }
  if( fd<0 ){
    fd = robust_open(path, openFlags, 0);
    terrno = errno;
    if( fd<0 && errno==ENOENT && islockfile ){
      if( proxyCreateLockPath(path) == SQLITE_OK ){
        fd = robust_open(path, openFlags, 0);
      }
    }
  }
  if( fd<0 ){
    openFlags = O_RDONLY;
    fd = robust_open(path, openFlags, 0);
    terrno = errno;
  }
  if( fd<0 ){
    if( islockfile ){
      return SQLITE_BUSY;
    }
    switch (terrno) {
      case EACCES:
        return SQLITE_PERM;
      case EIO: 
        return SQLITE_IOERR_LOCK; /* even though it is the conch */
      default:
        return SQLITE_CANTOPEN_BKPT;
    }
  }
  
  pNew = (unixFile *)sqlite3_malloc(sizeof(*pNew));
  if( pNew==NULL ){
    rc = SQLITE_NOMEM;
    goto end_create_proxy;
  }
  memset(pNew, 0, sizeof(unixFile));
  pNew->openFlags = openFlags;
  memset(&dummyVfs, 0, sizeof(dummyVfs));
  dummyVfs.pAppData = (void*)&autolockIoFinder;
  dummyVfs.zName = "dummy";
  pUnused->fd = fd;
  pUnused->flags = openFlags;
  pNew->pUnused = pUnused;
  
  rc = fillInUnixFile(&dummyVfs, fd, (sqlite3_file*)pNew, path, 0);
  if( rc==SQLITE_OK ){
    *ppFile = pNew;
    return SQLITE_OK;
  }
end_create_proxy:    
  robust_close(pNew, fd, __LINE__);
  sqlite3_free(pNew);
  sqlite3_free(pUnused);
  return rc;
}

#ifdef SQLITE_TEST
/* simulate multiple hosts by creating unique hostid file paths */
int sqlite3_hostid_num = 0;
#endif

#define PROXY_HOSTIDLEN    16  /* conch file host id length */

/* Not always defined in the headers as it ought to be */
extern int gethostuuid(uuid_t id, const struct timespec *wait);

/* get the host ID via gethostuuid(), pHostID must point to PROXY_HOSTIDLEN 
** bytes of writable memory.
*/
static int proxyGetHostID(unsigned char *pHostID, int *pError){
  assert(PROXY_HOSTIDLEN == sizeof(uuid_t));
  memset(pHostID, 0, PROXY_HOSTIDLEN);
#if defined(__MAX_OS_X_VERSION_MIN_REQUIRED)\
               && __MAC_OS_X_VERSION_MIN_REQUIRED<1050
  {
    static const struct timespec timeout = {1, 0}; /* 1 sec timeout */
    if( gethostuuid(pHostID, &timeout) ){
      int err = errno;
      if( pError ){
        *pError = err;
      }
      return SQLITE_IOERR;
    }
  }
#else
  UNUSED_PARAMETER(pError);
#endif
#ifdef SQLITE_TEST
  /* simulate multiple hosts by creating unique hostid file paths */
  if( sqlite3_hostid_num != 0){
    pHostID[0] = (char)(pHostID[0] + (char)(sqlite3_hostid_num & 0xFF));
  }
#endif
  
  return SQLITE_OK;
}

/* The conch file contains the header, host id and lock file path
 */
#define PROXY_CONCHVERSION 2   /* 1-byte header, 16-byte host id, path */
#define PROXY_HEADERLEN    1   /* conch file header length */
#define PROXY_PATHINDEX    (PROXY_HEADERLEN+PROXY_HOSTIDLEN)
#define PROXY_MAXCONCHLEN  (PROXY_HEADERLEN+PROXY_HOSTIDLEN+MAXPATHLEN)

/* 
** Takes an open conch file, copies the contents to a new path and then moves 
** it back.  The newly created file's file descriptor is assigned to the
** conch file structure and finally the original conch file descriptor is 
** closed.  Returns zero if successful.
**
** 获取打开的conch文件，将内容复制到新路径，然后将其移回。
** 新创建的文件的文件描述符被分配给conch文件结构，最后关闭原始的
** conch文件描述符。 如果成功，返回零。
**
*/
static int proxyBreakConchLock(unixFile *pFile, uuid_t myHostID){
  proxyLockingContext *pCtx = (proxyLockingContext *)pFile->lockingContext; 
  unixFile *conchFile = pCtx->conchFile;
  char tPath[MAXPATHLEN];
  char buf[PROXY_MAXCONCHLEN];
  char *cPath = pCtx->conchFilePath;
  size_t readLen = 0;
  size_t pathLen = 0;
  char errmsg[64] = "";
  int fd = -1;
  int rc = -1;
  UNUSED_PARAMETER(myHostID);

  /* create a new path by replace the trailing '-conch' with '-break' */
  pathLen = strlcpy(tPath, cPath, MAXPATHLEN);
  if( pathLen>MAXPATHLEN || pathLen<6 || 
     (strlcpy(&tPath[pathLen-5], "break", 6) != 5) ){
    sqlite3_snprintf(sizeof(errmsg),errmsg,"path error (len %d)",(int)pathLen);
    goto end_breaklock;
  }
  /* read the conch content */
  readLen = osPread(conchFile->h, buf, PROXY_MAXCONCHLEN, 0);
  if( readLen<PROXY_PATHINDEX ){
    sqlite3_snprintf(sizeof(errmsg),errmsg,"read error (len %d)",(int)readLen);
    goto end_breaklock;
  }
  /* write it out to the temporary break file */
  fd = robust_open(tPath, (O_RDWR|O_CREAT|O_EXCL), 0);
  if( fd<0 ){
    sqlite3_snprintf(sizeof(errmsg), errmsg, "create failed (%d)", errno);
    goto end_breaklock;
  }
  if( osPwrite(fd, buf, readLen, 0) != (ssize_t)readLen ){
    sqlite3_snprintf(sizeof(errmsg), errmsg, "write failed (%d)", errno);
    goto end_breaklock;
  }
  if( rename(tPath, cPath) ){
    sqlite3_snprintf(sizeof(errmsg), errmsg, "rename failed (%d)", errno);
    goto end_breaklock;
  }
  rc = 0;
  fprintf(stderr, "broke stale lock on %s\n", cPath);
  robust_close(pFile, conchFile->h, __LINE__);
  conchFile->h = fd;
  conchFile->openFlags = O_RDWR | O_CREAT;

end_breaklock:
  if( rc ){
    if( fd>=0 ){
      osUnlink(tPath);
      robust_close(pFile, fd, __LINE__);
    }
    fprintf(stderr, "failed to break stale lock on %s, %s\n", cPath, errmsg);
  }
  return rc;
}

/* Take the requested lock on the conch file and break a stale lock if the 
** host id matches.
*/
static int proxyConchLock(unixFile *pFile, uuid_t myHostID, int lockType){
  proxyLockingContext *pCtx = (proxyLockingContext *)pFile->lockingContext; 
  unixFile *conchFile = pCtx->conchFile;
  int rc = SQLITE_OK;
  int nTries = 0;
  struct timespec conchModTime;
  
  memset(&conchModTime, 0, sizeof(conchModTime));
  do {
    rc = conchFile->pMethod->xLock((sqlite3_file*)conchFile, lockType);
    nTries ++;
    if( rc==SQLITE_BUSY ){
      /* If the lock failed (busy):
       * 1st try: get the mod time of the conch, wait 0.5s and try again. 
       * 2nd try: fail if the mod time changed or host id is different, wait 
       *           10 sec and try again
       * 3rd try: break the lock unless the mod time has changed.
       * 如果锁定失败（忙）：
       * 第一次尝试：获取conch的mod时间，等待0.5s，然后重试。
       * 第二次尝试：失败如果mod时间改变或主机ID不同，等待10秒，然后重试
       * 第三尝试：打破锁，除非mod时间已经改变。
       */
      struct stat buf;
      if( osFstat(conchFile->h, &buf) ){
        pFile->lastErrno = errno;
        return SQLITE_IOERR_LOCK;
      }
      
      if( nTries==1 ){
        conchModTime = buf.st_mtimespec;
        usleep(500000); /* wait 0.5 sec and try the lock again*/
        continue;  
      }

      assert( nTries>1 );
      if( conchModTime.tv_sec != buf.st_mtimespec.tv_sec || 
         conchModTime.tv_nsec != buf.st_mtimespec.tv_nsec ){
        return SQLITE_BUSY;
      }
      
      if( nTries==2 ){  
        char tBuf[PROXY_MAXCONCHLEN];
        int len = osPread(conchFile->h, tBuf, PROXY_MAXCONCHLEN, 0);
        if( len<0 ){
          pFile->lastErrno = errno;
          return SQLITE_IOERR_LOCK;
        }
        if( len>PROXY_PATHINDEX && tBuf[0]==(char)PROXY_CONCHVERSION){
          /* don't break the lock if the host id doesn't match */
          if( 0!=memcmp(&tBuf[PROXY_HEADERLEN], myHostID, PROXY_HOSTIDLEN) ){
            return SQLITE_BUSY;
          }
        }else{
          /* don't break the lock on short read or a version mismatch */
          return SQLITE_BUSY;
        }
        usleep(10000000); /* wait 10 sec and try the lock again */
        continue; 
      }
      
      assert( nTries==3 );
      if( 0==proxyBreakConchLock(pFile, myHostID) ){
        rc = SQLITE_OK;
        if( lockType==EXCLUSIVE_LOCK ){
          rc = conchFile->pMethod->xLock((sqlite3_file*)conchFile, SHARED_LOCK);          
        }
        if( !rc ){
          rc = conchFile->pMethod->xLock((sqlite3_file*)conchFile, lockType);
        }
      }
    }
  } while( rc==SQLITE_BUSY && nTries<3 );
  
  return rc;
}

/* Takes the conch by taking a shared lock and read the contents conch, if 
** lockPath is non-NULL, the host ID and lock file path must match.  A NULL 
** lockPath means that the lockPath in the conch file will be used if the 
** host IDs match, or a new lock path will be generated automatically 
** and written to the conch file.
*/
static int proxyTakeConch(unixFile *pFile){
  proxyLockingContext *pCtx = (proxyLockingContext *)pFile->lockingContext; 
  
  if( pCtx->conchHeld!=0 ){
    return SQLITE_OK;
  }else{
    unixFile *conchFile = pCtx->conchFile;
    uuid_t myHostID;
    int pError = 0;
    char readBuf[PROXY_MAXCONCHLEN];
    char lockPath[MAXPATHLEN];
    char *tempLockPath = NULL;
    int rc = SQLITE_OK;
    int createConch = 0;
    int hostIdMatch = 0;
    int readLen = 0;
    int tryOldLockPath = 0;
    int forceNewLockPath = 0;
    
    OSTRACE(("TAKECONCH  %d for %s pid=%d\n", conchFile->h,
             (pCtx->lockProxyPath ? pCtx->lockProxyPath : ":auto:"), getpid()));

    rc = proxyGetHostID(myHostID, &pError);
    if( (rc&0xff)==SQLITE_IOERR ){
      pFile->lastErrno = pError;
      goto end_takeconch;
    }
    rc = proxyConchLock(pFile, myHostID, SHARED_LOCK);
    if( rc!=SQLITE_OK ){
      goto end_takeconch;
    }
    /* read the existing conch file */
    readLen = seekAndRead((unixFile*)conchFile, 0, readBuf, PROXY_MAXCONCHLEN);
    if( readLen<0 ){
      /* I/O error: lastErrno set by seekAndRead */
      pFile->lastErrno = conchFile->lastErrno;
      rc = SQLITE_IOERR_READ;
      goto end_takeconch;
    }else if( readLen<=(PROXY_HEADERLEN+PROXY_HOSTIDLEN) || 
             readBuf[0]!=(char)PROXY_CONCHVERSION ){
      /* a short read or version format mismatch means we need to create a new 
      ** conch file. 
      */
      createConch = 1;
    }
    /* if the host id matches and the lock path already exists in the conch
    ** we'll try to use the path there, if we can't open that path, we'll 
    ** retry with a new auto-generated path 
    */
    do { /* in case we need to try again for an :auto: named lock file */

      if( !createConch && !forceNewLockPath ){
        hostIdMatch = !memcmp(&readBuf[PROXY_HEADERLEN], myHostID, 
                                  PROXY_HOSTIDLEN);
        /* if the conch has data compare the contents */
        if( !pCtx->lockProxyPath ){
          /* for auto-named local lock file, just check the host ID and we'll
           ** use the local lock file path that's already in there
           */
          if( hostIdMatch ){
            size_t pathLen = (readLen - PROXY_PATHINDEX);
            
            if( pathLen>=MAXPATHLEN ){
              pathLen=MAXPATHLEN-1;
            }
            memcpy(lockPath, &readBuf[PROXY_PATHINDEX], pathLen);
            lockPath[pathLen] = 0;
            tempLockPath = lockPath;
            tryOldLockPath = 1;
            /* create a copy of the lock path if the conch is taken */
            goto end_takeconch;
          }
        }else if( hostIdMatch
               && !strncmp(pCtx->lockProxyPath, &readBuf[PROXY_PATHINDEX],
                           readLen-PROXY_PATHINDEX)
        ){
          /* conch host and lock path match */
          goto end_takeconch; 
        }
      }
      
      /* if the conch isn't writable and doesn't match, we can't take it */
      if( (conchFile->openFlags&O_RDWR) == 0 ){
        rc = SQLITE_BUSY;
        goto end_takeconch;
      }
      
      /* either the conch didn't match or we need to create a new one */
      if( !pCtx->lockProxyPath ){
        proxyGetLockPath(pCtx->dbPath, lockPath, MAXPATHLEN);
        tempLockPath = lockPath;
        /* create a copy of the lock path _only_ if the conch is taken */
      }
      
      /* update conch with host and path (this will fail if other process
      ** has a shared lock already), if the host id matches, use the big
      ** stick.
      */
      futimes(conchFile->h, NULL);
      if( hostIdMatch && !createConch ){
        if( conchFile->pInode && conchFile->pInode->nShared>1 ){
          /* We are trying for an exclusive lock but another thread in this
           ** same process is still holding a shared lock. */
          rc = SQLITE_BUSY;
        } else {          
          rc = proxyConchLock(pFile, myHostID, EXCLUSIVE_LOCK);
        }
      }else{
        rc = conchFile->pMethod->xLock((sqlite3_file*)conchFile, EXCLUSIVE_LOCK);
      }
      if( rc==SQLITE_OK ){
        char writeBuffer[PROXY_MAXCONCHLEN];
        int writeSize = 0;
        
        writeBuffer[0] = (char)PROXY_CONCHVERSION;
        memcpy(&writeBuffer[PROXY_HEADERLEN], myHostID, PROXY_HOSTIDLEN);
        if( pCtx->lockProxyPath!=NULL ){
          strlcpy(&writeBuffer[PROXY_PATHINDEX], pCtx->lockProxyPath, MAXPATHLEN);
        }else{
          strlcpy(&writeBuffer[PROXY_PATHINDEX], tempLockPath, MAXPATHLEN);
        }
        writeSize = PROXY_PATHINDEX + strlen(&writeBuffer[PROXY_PATHINDEX]);
        robust_ftruncate(conchFile->h, writeSize);
        rc = unixWrite((sqlite3_file *)conchFile, writeBuffer, writeSize, 0);
        fsync(conchFile->h);
        /* If we created a new conch file (not just updated the contents of a 
         ** valid conch file), try to match the permissions of the database 
         */
        if( rc==SQLITE_OK && createConch ){
          struct stat buf;
          int err = osFstat(pFile->h, &buf);
          if( err==0 ){
            mode_t cmode = buf.st_mode&(S_IRUSR|S_IWUSR | S_IRGRP|S_IWGRP |
                                        S_IROTH|S_IWOTH);
            /* try to match the database file R/W permissions, ignore failure */
#ifndef SQLITE_PROXY_DEBUG
            osFchmod(conchFile->h, cmode);
#else
            do{
              rc = osFchmod(conchFile->h, cmode);
            }while( rc==(-1) && errno==EINTR );
            if( rc!=0 ){
              int code = errno;
              fprintf(stderr, "fchmod %o FAILED with %d %s\n",
                      cmode, code, strerror(code));
            } else {
              fprintf(stderr, "fchmod %o SUCCEDED\n",cmode);
            }
          }else{
            int code = errno;
            fprintf(stderr, "STAT FAILED[%d] with %d %s\n", 
                    err, code, strerror(code));
#endif
          }
        }
      }
      conchFile->pMethod->xUnlock((sqlite3_file*)conchFile, SHARED_LOCK);
      
    end_takeconch:
      OSTRACE(("TRANSPROXY: CLOSE  %d\n", pFile->h));
      if( rc==SQLITE_OK && pFile->openFlags ){
        int fd;
        if( pFile->h>=0 ){
          robust_close(pFile, pFile->h, __LINE__);
        }
        pFile->h = -1;
        fd = robust_open(pCtx->dbPath, pFile->openFlags, 0);
        OSTRACE(("TRANSPROXY: OPEN  %d\n", fd));
        if( fd>=0 ){
          pFile->h = fd;
        }else{
          rc=SQLITE_CANTOPEN_BKPT; /* SQLITE_BUSY? proxyTakeConch called
           during locking */
        }
      }
      if( rc==SQLITE_OK && !pCtx->lockProxy ){
        char *path = tempLockPath ? tempLockPath : pCtx->lockProxyPath;
        rc = proxyCreateUnixFile(path, &pCtx->lockProxy, 1);
        if( rc!=SQLITE_OK && rc!=SQLITE_NOMEM && tryOldLockPath ){
          /* we couldn't create the proxy lock file with the old lock file path
           ** so try again via auto-naming 
           */
          forceNewLockPath = 1;
          tryOldLockPath = 0;
          continue; /* go back to the do {} while start point, try again */
        }
      }
      if( rc==SQLITE_OK ){
        /* Need to make a copy of path if we extracted the value
         ** from the conch file or the path was allocated on the stack
         */
        if( tempLockPath ){
          pCtx->lockProxyPath = sqlite3DbStrDup(0, tempLockPath);
          if( !pCtx->lockProxyPath ){
            rc = SQLITE_NOMEM;
          }
        }
      }
      if( rc==SQLITE_OK ){
        pCtx->conchHeld = 1;
        
        if( pCtx->lockProxy->pMethod == &afpIoMethods ){
          afpLockingContext *afpCtx;
          afpCtx = (afpLockingContext *)pCtx->lockProxy->lockingContext;
          afpCtx->dbPath = pCtx->lockProxyPath;
        }
      } else {
        conchFile->pMethod->xUnlock((sqlite3_file*)conchFile, NO_LOCK);
      }
      OSTRACE(("TAKECONCH  %d %s\n", conchFile->h,
               rc==SQLITE_OK?"ok":"failed"));
      return rc;
    } while (1); /* in case we need to retry the :auto: lock file - 
                 ** we should never get here except via the 'continue' call. */
  }
}

/*
** If pFile holds a lock on a conch file, then release that lock.
*/
static int proxyReleaseConch(unixFile *pFile){
  int rc = SQLITE_OK;         /* Subroutine return code */
  proxyLockingContext *pCtx;  /* The locking context for the proxy lock */
  unixFile *conchFile;        /* Name of the conch file */

  pCtx = (proxyLockingContext *)pFile->lockingContext;
  conchFile = pCtx->conchFile;
  OSTRACE(("RELEASECONCH  %d for %s pid=%d\n", conchFile->h,
           (pCtx->lockProxyPath ? pCtx->lockProxyPath : ":auto:"), 
           getpid()));
  if( pCtx->conchHeld>0 ){
    rc = conchFile->pMethod->xUnlock((sqlite3_file*)conchFile, NO_LOCK);
  }
  pCtx->conchHeld = 0;
  OSTRACE(("RELEASECONCH  %d %s\n", conchFile->h,
           (rc==SQLITE_OK ? "ok" : "failed")));
  return rc;
}

/*
** Given the name of a database file, compute the name of its conch file.
** Store the conch filename in memory obtained from sqlite3_malloc().
** Make *pConchPath point to the new name.  Return SQLITE_OK on success
** or SQLITE_NOMEM if unable to obtain memory.
**
** The caller is responsible for ensuring that the allocated memory
** space is eventually freed.
**
** *pConchPath is set to NULL if a memory allocation error occurs.
**
** 对于给定的数据库文件名称，计算其conch文件的名称。 
** 将从sqlite3_malloc（）获得的conch文件名存储在内存中。 Make * pConchPath指向新名称。 
** 返回SQLITE_OK成功或SQLITE_NOMEM如果无法获得内存。
**
** 调用者负责确保分配的内存空间最终被释放。
**
** 如果发生内存分配错误，*pConchPath设置为NULL。
**
*/
static int proxyCreateConchPathname(char *dbPath, char **pConchPath){
  int i;                        /* Loop counter */
  int len = (int)strlen(dbPath); /* Length of database filename - dbPath */
  char *conchPath;              /* buffer in which to construct conch name */

  /* Allocate space for the conch filename and initialize the name to
  ** the name of the original database file. */  
  *pConchPath = conchPath = (char *)sqlite3_malloc(len + 8);
  if( conchPath==0 ){
    return SQLITE_NOMEM;
  }
  memcpy(conchPath, dbPath, len+1);
  
  /* now insert a "." before the last / character */
  for( i=(len-1); i>=0; i-- ){
    if( conchPath[i]=='/' ){
      i++;
      break;
    }
  }
  conchPath[i]='.';
  while ( i<len ){
    conchPath[i+1]=dbPath[i];
    i++;
  }

  /* append the "-conch" suffix to the file */
  memcpy(&conchPath[i+1], "-conch", 7);
  assert( (int)strlen(conchPath) == len+7 );

  return SQLITE_OK;
}


/* Takes a fully configured proxy locking-style unix file and switches
** the local lock file path 
*/
static int switchLockProxyPath(unixFile *pFile, const char *path) {
  proxyLockingContext *pCtx = (proxyLockingContext*)pFile->lockingContext;
  char *oldPath = pCtx->lockProxyPath;
  int rc = SQLITE_OK;

  if( pFile->eFileLock!=NO_LOCK ){
    return SQLITE_BUSY;
  }  

  /* nothing to do if the path is NULL, :auto: or matches the existing path */
  if( !path || path[0]=='\0' || !strcmp(path, ":auto:") ||
    (oldPath && !strncmp(oldPath, path, MAXPATHLEN)) ){
    return SQLITE_OK;
  }else{
    unixFile *lockProxy = pCtx->lockProxy;
    pCtx->lockProxy=NULL;
    pCtx->conchHeld = 0;
    if( lockProxy!=NULL ){
      rc=lockProxy->pMethod->xClose((sqlite3_file *)lockProxy);
      if( rc ) return rc;
      sqlite3_free(lockProxy);
    }
    sqlite3_free(oldPath);
    pCtx->lockProxyPath = sqlite3DbStrDup(0, path);
  }
  
  return rc;
}

/*
** pFile is a file that has been opened by a prior xOpen call.  dbPath
** is a string buffer at least MAXPATHLEN+1 characters in size.
**
** This routine find the filename associated with pFile and writes it
** int dbPath.
**
** pFile是由先前的xOpen调用打开的文件。 dbPath是一个字符串缓冲区，
** 大小至少为MAXPATHLEN + 1个字符。
**
** 此例程查找与pFile相关联的文件名，并将其写入int dbPath。
**
*/
static int proxyGetDbPathForUnixFile(unixFile *pFile, char *dbPath){
#if defined(__APPLE__)
  if( pFile->pMethod == &afpIoMethods ){
    /* afp style keeps a reference to the db path in the filePath field 
    ** of the struct */
    assert( (int)strlen((char*)pFile->lockingContext)<=MAXPATHLEN );
    strlcpy(dbPath, ((afpLockingContext *)pFile->lockingContext)->dbPath, MAXPATHLEN);
  } else
#endif
  if( pFile->pMethod == &dotlockIoMethods ){
    /* dot lock style uses the locking context to store the dot lock
    ** file path */
    int len = strlen((char *)pFile->lockingContext) - strlen(DOTLOCK_SUFFIX);
    memcpy(dbPath, (char *)pFile->lockingContext, len + 1);
  }else{
    /* all other styles use the locking context to store the db file path */
    assert( strlen((char*)pFile->lockingContext)<=MAXPATHLEN );
    strlcpy(dbPath, (char *)pFile->lockingContext, MAXPATHLEN);
  }
  return SQLITE_OK;
}

/*
** Takes an already filled in unix file and alters it so all file locking 
** will be performed on the local proxy lock file.  The following fields
** are preserved in the locking context so that they can be restored and 
** the unix structure properly cleaned up at close time:
**  ->lockingContext
**  ->pMethod
**
** 获取已填充的unix文件并更改它，因此所有文件锁定将在本地代理
** 锁定文件上执行。 以下字段保留在锁定上下文中，以便可以恢复它们，
** 并在关闭时正确清理unix结构：
** - > lockingContext
** - > pMethod
**
*/
static int proxyTransformUnixFile(unixFile *pFile, const char *path) {
  proxyLockingContext *pCtx;
  char dbPath[MAXPATHLEN+1];       /* Name of the database file */
  char *lockPath=NULL;
  int rc = SQLITE_OK;
  
  if( pFile->eFileLock!=NO_LOCK ){
    return SQLITE_BUSY;
  }
  proxyGetDbPathForUnixFile(pFile, dbPath);
  if( !path || path[0]=='\0' || !strcmp(path, ":auto:") ){
    lockPath=NULL;
  }else{
    lockPath=(char *)path;
  }
  
  OSTRACE(("TRANSPROXY  %d for %s pid=%d\n", pFile->h,
           (lockPath ? lockPath : ":auto:"), getpid()));

  pCtx = sqlite3_malloc( sizeof(*pCtx) );
  if( pCtx==0 ){
    return SQLITE_NOMEM;
  }
  memset(pCtx, 0, sizeof(*pCtx));

  rc = proxyCreateConchPathname(dbPath, &pCtx->conchFilePath);
  if( rc==SQLITE_OK ){
    rc = proxyCreateUnixFile(pCtx->conchFilePath, &pCtx->conchFile, 0);
    if( rc==SQLITE_CANTOPEN && ((pFile->openFlags&O_RDWR) == 0) ){
      /* if (a) the open flags are not O_RDWR, (b) the conch isn't there, and
      ** (c) the file system is read-only, then enable no-locking access.
      ** Ugh, since O_RDONLY==0x0000 we test for !O_RDWR since unixOpen asserts
      ** that openFlags will have only one of O_RDONLY or O_RDWR.
      */
      struct statfs fsInfo;
      struct stat conchInfo;
      int goLockless = 0;

      if( osStat(pCtx->conchFilePath, &conchInfo) == -1 ) {
        int err = errno;
        if( (err==ENOENT) && (statfs(dbPath, &fsInfo) != -1) ){
          goLockless = (fsInfo.f_flags&MNT_RDONLY) == MNT_RDONLY;
        }
      }
      if( goLockless ){
        pCtx->conchHeld = -1; /* read only FS/ lockless */
        rc = SQLITE_OK;
      }
    }
  }  
  if( rc==SQLITE_OK && lockPath ){
    pCtx->lockProxyPath = sqlite3DbStrDup(0, lockPath);
  }

  if( rc==SQLITE_OK ){
    pCtx->dbPath = sqlite3DbStrDup(0, dbPath);
    if( pCtx->dbPath==NULL ){
      rc = SQLITE_NOMEM;
    }
  }
  if( rc==SQLITE_OK ){
    /* all memory is allocated, proxys are created and assigned, 
    ** switch the locking context and pMethod then return.
    */
    pCtx->oldLockingContext = pFile->lockingContext;
    pFile->lockingContext = pCtx;
    pCtx->pOldMethod = pFile->pMethod;
    pFile->pMethod = &proxyIoMethods;
  }else{
    if( pCtx->conchFile ){ 
      pCtx->conchFile->pMethod->xClose((sqlite3_file *)pCtx->conchFile);
      sqlite3_free(pCtx->conchFile);
    }
    sqlite3DbFree(0, pCtx->lockProxyPath);
    sqlite3_free(pCtx->conchFilePath); 
    sqlite3_free(pCtx);
  }
  OSTRACE(("TRANSPROXY  %d %s\n", pFile->h,
           (rc==SQLITE_OK ? "ok" : "failed")));
  return rc;
}


/*
** This routine handles sqlite3_file_control() calls that are specific
** to proxy locking.
**
** 此例程处理特定于代理锁定的sqlite3_file_control（）调用。
**
*/
static int proxyFileControl(sqlite3_file *id, int op, void *pArg){
  switch( op ){
    case SQLITE_GET_LOCKPROXYFILE: {
      unixFile *pFile = (unixFile*)id;
      if( pFile->pMethod == &proxyIoMethods ){
        proxyLockingContext *pCtx = (proxyLockingContext*)pFile->lockingContext;
        proxyTakeConch(pFile);
        if( pCtx->lockProxyPath ){
          *(const char **)pArg = pCtx->lockProxyPath;
        }else{
          *(const char **)pArg = ":auto: (not held)";
        }
      } else {
        *(const char **)pArg = NULL;
      }
      return SQLITE_OK;
    }
    case SQLITE_SET_LOCKPROXYFILE: {
      unixFile *pFile = (unixFile*)id;
      int rc = SQLITE_OK;
      int isProxyStyle = (pFile->pMethod == &proxyIoMethods);
      if( pArg==NULL || (const char *)pArg==0 ){
        if( isProxyStyle ){
          /* turn off proxy locking - not supported */
          rc = SQLITE_ERROR /*SQLITE_PROTOCOL? SQLITE_MISUSE?*/;
        }else{
          /* turn off proxy locking - already off - NOOP */
          rc = SQLITE_OK;
        }
      }else{
        const char *proxyPath = (const char *)pArg;
        if( isProxyStyle ){
          proxyLockingContext *pCtx = 
            (proxyLockingContext*)pFile->lockingContext;
          if( !strcmp(pArg, ":auto:") 
           || (pCtx->lockProxyPath &&
               !strncmp(pCtx->lockProxyPath, proxyPath, MAXPATHLEN))
          ){
            rc = SQLITE_OK;
          }else{
            rc = switchLockProxyPath(pFile, proxyPath);
          }
        }else{
          /* turn on proxy file locking */
          rc = proxyTransformUnixFile(pFile, proxyPath);
        }
      }
      return rc;
    }
    default: {
      assert( 0 );  /* The call assures that only valid opcodes are sent */
    }
  }
  /*NOTREACHED*/
  return SQLITE_ERROR;
}

/*
** Within this division (the proxying locking implementation) the procedures
** above this point are all utilities.  The lock-related methods of the
** proxy-locking sqlite3_io_method object follow.
**
** 在这个部门（代理锁定实现）中，上面的过程都是实用程序。 
** 代理锁定sqlite3_io_method对象的锁相关方法如下。
**
*/


/*
** This routine checks if there is a RESERVED lock held on the specified
** file by this or any other process. If such a lock is held, set *pResOut
** to a non-zero value otherwise *pResOut is set to zero.  The return value
** is set to SQLITE_OK unless an I/O error occurs during lock checking.
**
** 此例程检查此过程或任何其他过程是否在指定的文件上保留了RESERVED锁。 
** 如果保持这样的锁定，将* pResOut设置为非零值，否则将* pResOut设置为零。 
** 返回值设置为SQLITE_OK，除非在锁定检查期间发生I / O错误。
**
*/
static int proxyCheckReservedLock(sqlite3_file *id, int *pResOut) {
  unixFile *pFile = (unixFile*)id;
  int rc = proxyTakeConch(pFile);
  if( rc==SQLITE_OK ){
    proxyLockingContext *pCtx = (proxyLockingContext *)pFile->lockingContext;
    if( pCtx->conchHeld>0 ){
      unixFile *proxy = pCtx->lockProxy;
      return proxy->pMethod->xCheckReservedLock((sqlite3_file*)proxy, pResOut);
    }else{ /* conchHeld < 0 is lockless */
      pResOut=0;
    }
  }
  return rc;
}

/*
** Lock the file with the lock specified by parameter eFileLock - one
** of the following:
**
**     (1) SHARED_LOCK
**     (2) RESERVED_LOCK
**     (3) PENDING_LOCK
**     (4) EXCLUSIVE_LOCK
**
** Sometimes when requesting one lock state, additional lock states
** are inserted in between.  The locking might fail on one of the later
** transitions leaving the lock state different from what it started but
** still short of its goal.  The following chart shows the allowed
** transitions and the inserted intermediate states:
**
**    UNLOCKED -> SHARED
**    SHARED -> RESERVED
**    SHARED -> (PENDING) -> EXCLUSIVE
**    RESERVED -> (PENDING) -> EXCLUSIVE
**    PENDING -> EXCLUSIVE
**
** This routine will only increase a lock.  Use the sqlite3OsUnlock()
** routine to lower a locking level.
*/
static int proxyLock(sqlite3_file *id, int eFileLock) {
  unixFile *pFile = (unixFile*)id;
  int rc = proxyTakeConch(pFile);
  if( rc==SQLITE_OK ){
    proxyLockingContext *pCtx = (proxyLockingContext *)pFile->lockingContext;
    if( pCtx->conchHeld>0 ){
      unixFile *proxy = pCtx->lockProxy;
      rc = proxy->pMethod->xLock((sqlite3_file*)proxy, eFileLock);
      pFile->eFileLock = proxy->eFileLock;
    }else{
      /* conchHeld < 0 is lockless */
    }
  }
  return rc;
}


/*
** Lower the locking level on file descriptor pFile to eFileLock.  eFileLock
** must be either NO_LOCK or SHARED_LOCK.
**
** If the locking level of the file descriptor is already at or below
** the requested locking level, this routine is a no-op.
*/
static int proxyUnlock(sqlite3_file *id, int eFileLock) {
  unixFile *pFile = (unixFile*)id;
  int rc = proxyTakeConch(pFile);
  if( rc==SQLITE_OK ){
    proxyLockingContext *pCtx = (proxyLockingContext *)pFile->lockingContext;
    if( pCtx->conchHeld>0 ){
      unixFile *proxy = pCtx->lockProxy;
      rc = proxy->pMethod->xUnlock((sqlite3_file*)proxy, eFileLock);
      pFile->eFileLock = proxy->eFileLock;
    }else{
      /* conchHeld < 0 is lockless */
    }
  }
  return rc;
}

/*
** Close a file that uses proxy locks.
*/
static int proxyClose(sqlite3_file *id) {
  if( id ){
    unixFile *pFile = (unixFile*)id;
    proxyLockingContext *pCtx = (proxyLockingContext *)pFile->lockingContext;
    unixFile *lockProxy = pCtx->lockProxy;
    unixFile *conchFile = pCtx->conchFile;
    int rc = SQLITE_OK;
    
    if( lockProxy ){
      rc = lockProxy->pMethod->xUnlock((sqlite3_file*)lockProxy, NO_LOCK);
      if( rc ) return rc;
      rc = lockProxy->pMethod->xClose((sqlite3_file*)lockProxy);
      if( rc ) return rc;
      sqlite3_free(lockProxy);
      pCtx->lockProxy = 0;
    }
    if( conchFile ){
      if( pCtx->conchHeld ){
        rc = proxyReleaseConch(pFile);
        if( rc ) return rc;
      }
      rc = conchFile->pMethod->xClose((sqlite3_file*)conchFile);
      if( rc ) return rc;
      sqlite3_free(conchFile);
    }
    sqlite3DbFree(0, pCtx->lockProxyPath);
    sqlite3_free(pCtx->conchFilePath);
    sqlite3DbFree(0, pCtx->dbPath);
    /* restore the original locking context and pMethod then close it */
    pFile->lockingContext = pCtx->oldLockingContext;
    pFile->pMethod = pCtx->pOldMethod;
    sqlite3_free(pCtx);
    return pFile->pMethod->xClose(id);
  }
  return SQLITE_OK;
}



#endif /* defined(__APPLE__) && SQLITE_ENABLE_LOCKING_STYLE */
/*
** The proxy locking style is intended for use with AFP filesystems.
** And since AFP is only supported on MacOSX, the proxy locking is also
** restricted to MacOSX.
**
** 代理锁定样式与AFP文件系统一起使用。 由于AFP仅在MacOSX上受支持，
** 因此代理锁定也仅限于MacOSX。
** 
**
******************* End of the proxy lock implementation **********************
******************************************************************************/

/*
** Initialize the operating system interface.
**
** This routine registers all VFS implementations for unix-like operating
** systems.  This routine, and the sqlite3_os_end() routine that follows,
** should be the only routines in this file that are visible from other
** files.
**
** This routine is called once during SQLite initialization and by a
** single thread.  The memory allocation and mutex subsystems have not
** necessarily been initialized when this routine is called, and so they
** should not be used.
**
** 初始化操作系统接口。
**
** 此例程注册所有VFS实施为unix类操作系统。 此例程和后面的sqlite3_os_end（）
** 例程应该是此文件中从其他文件可见的唯一例程。
**
** 此例程在SQLite初始化期间调用一次，由单个线程调用。 当调用此例程时，
** 内存分配和互斥子系统不必初始化，因此不应使用它们。
**
*/
int sqlite3_os_init(void){ 
  /* 
  ** The following macro defines an initializer for an sqlite3_vfs object.
  ** The name of the VFS is NAME.  The pAppData is a pointer to a pointer
  ** to the "finder" function.  (pAppData is a pointer to a pointer because
  ** silly C90 rules prohibit a void* from being cast to a function pointer
  ** and so we have to go through the intermediate pointer to avoid problems
  ** when compiling with -pedantic-errors on GCC.)
  **
  ** The FINDER parameter to this macro is the name of the pointer to the
  ** finder-function.  The finder-function returns a pointer to the
  ** sqlite_io_methods object that implements the desired locking
  ** behaviors.  See the division above that contains the IOMETHODS
  ** macro for addition information on finder-functions.
  **
  ** Most finders simply return a pointer to a fixed sqlite3_io_methods
  ** object.  But the "autolockIoFinder" available on MacOSX does a little
  ** more than that; it looks at the filesystem type that hosts the 
  ** database file and tries to choose an locking method appropriate for
  ** that filesystem time.
  **
  ** 下列宏定义了sqlite3_vfs对象的初始化程序。 VFS的名称为NAME。 pAppData是指向
  ** “finder”函数的指针的指针。 （pAppData是指向指针的指针，因为愚蠢的C90规则
  ** 禁止将void *转换为函数指针，因此我们必须通过中间指针，以避免在GCC上使用
  ** -pedantic-errors编译时出现问题）。
  **
  ** 此宏的FINDER参数是finder函数的指针名称。 finder函数返回一个指向实现所需锁定
  ** 行为的sqlite_io_methods对象的指针。关于finder函数的附加信息，请参见上面包含
  ** IOMETHODS宏的部分。
  **
  ** 大多数finder只返回一个指向固定的sqlite3_io_methods对象的指针。但MacOSX上可用的
  ** “autolockIoFinder”会多一些，它会查看托管数据库文件的文件系统类型，并尝试
  ** 选择适合该文件系统时间的锁定方法。
  */
  #define UNIXVFS(VFSNAME, FINDER) {                        \
    3,                    /* iVersion */                    \
    sizeof(unixFile),     /* szOsFile */                    \
    MAX_PATHNAME,         /* mxPathname */                  \
    0,                    /* pNext */                       \
    VFSNAME,              /* zName */                       \
    (void*)&FINDER,       /* pAppData */                    \
    unixOpen,             /* xOpen */                       \
    unixDelete,           /* xDelete */                     \
    unixAccess,           /* xAccess */                     \
    unixFullPathname,     /* xFullPathname */               \
    unixDlOpen,           /* xDlOpen */                     \
    unixDlError,          /* xDlError */                    \
    unixDlSym,            /* xDlSym */                      \
    unixDlClose,          /* xDlClose */                    \
    unixRandomness,       /* xRandomness */                 \
    unixSleep,            /* xSleep */                      \
    unixCurrentTime,      /* xCurrentTime */                \
    unixGetLastError,     /* xGetLastError */               \
    unixCurrentTimeInt64, /* xCurrentTimeInt64 */           \
    unixSetSystemCall,    /* xSetSystemCall */              \
    unixGetSystemCall,    /* xGetSystemCall */              \
    unixNextSystemCall,   /* xNextSystemCall */             \
  }

  /*
  ** All default VFSes for unix are contained in the following array.
  **
  ** Note that the sqlite3_vfs.pNext field of the VFS object is modified
  ** by the SQLite core when the VFS is registered.  So the following
  ** array cannot be const.
  **
  ** UNIX的所有默认VFS包含在以下数组中。
  **
  ** 请注意，在注册VFS时，SQLite核心会修改VFS对象的sqlite3_vfs.pNext字段。
  ** 所以下面的数组不能是const。
  */
  static sqlite3_vfs aVfs[] = {
#if SQLITE_ENABLE_LOCKING_STYLE && (OS_VXWORKS || defined(__APPLE__))
    UNIXVFS("unix",          autolockIoFinder ),
#else
    UNIXVFS("unix",          posixIoFinder ),
#endif
    UNIXVFS("unix-none",     nolockIoFinder ),
    UNIXVFS("unix-dotfile",  dotlockIoFinder ),
    UNIXVFS("unix-excl",     posixIoFinder ),
#if OS_VXWORKS
    UNIXVFS("unix-namedsem", semIoFinder ),
#endif
#if SQLITE_ENABLE_LOCKING_STYLE
    UNIXVFS("unix-posix",    posixIoFinder ),
#if !OS_VXWORKS
    UNIXVFS("unix-flock",    flockIoFinder ),
#endif
#endif
#if SQLITE_ENABLE_LOCKING_STYLE && defined(__APPLE__)
    UNIXVFS("unix-afp",      afpIoFinder ),
    UNIXVFS("unix-nfs",      nfsIoFinder ),
    UNIXVFS("unix-proxy",    proxyIoFinder ),
#endif
  };
  unsigned int i;          /* Loop counter */

  /* Double-check that the aSyscall[] array has been constructed
  ** correctly.  See ticket [bb3a86e890c8e96ab] */
  assert( ArraySize(aSyscall)==22 );

  /* Register all VFSes defined in the aVfs[] array */
  for(i=0; i<(sizeof(aVfs)/sizeof(sqlite3_vfs)); i++){
    sqlite3_vfs_register(&aVfs[i], i==0);
  }
  return SQLITE_OK; 
}

/*
** Shutdown the operating system interface.
**
** Some operating systems might need to do some cleanup in this routine,
** to release dynamically allocated objects.  But not on unix.
** This routine is a no-op for unix.
**
** 关闭操作系统接口。
**
** 某些操作系统可能需要在此例程中进行一些清理，以释放动态分配的对象。
** 但不会在unix中进行。 所以这个例程是unix的无操作。
**
*/
int sqlite3_os_end(void){ 
  return SQLITE_OK; 
}
 
#endif /* SQLITE_OS_UNIX */
