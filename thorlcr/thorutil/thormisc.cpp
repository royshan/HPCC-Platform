/*##############################################################################

    Copyright (C) 2011 HPCC Systems.

    All rights reserved. This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
############################################################################## */

#ifndef _WIN32
#include <sys/types.h>
#include <dirent.h>
#endif

#include <stdio.h>
#include <time.h>

#include "jexcept.hpp"
#include "jfile.hpp"
#include "jmisc.hpp"
#include "jsocket.hpp"
#include "jmutex.hpp"

#include "commonext.hpp"
#include "dasds.hpp"
#include "dafdesc.hpp"

#include "thor.hpp"
#include "thorport.hpp"
#include "thormisc.hpp"
#include "thgraph.hpp"
#include "thbufdef.hpp"

#include "eclrtl.hpp"
#include "eclhelper.hpp"
#include "eclrtl_imp.hpp"
#include "rtlfield_imp.hpp"
#include "rtlds_imp.hpp"

namespace thormisc {  // Make sure we can't clash with generated versions or version check mechanism fails.
 #include "eclhelper_base.hpp" 
}

#define SDS_LOCK_TIMEOUT 30000

static IGroup *clusterGroup;
static IGroup *slaveGroup;
static IGroup *dfsGroup;
static ICommunicator *clusterComm;


mptag_t masterSlaveMpTag;
IPropertyTree *globals;
static IMPtagAllocator *ClusterMPAllocator = NULL;

MODULE_INIT(INIT_PRIORITY_STANDARD)
{
    globals = NULL;
    clusterGroup = NULL;
    slaveGroup = NULL;
    dfsGroup = NULL;
    clusterComm = NULL;
    ClusterMPAllocator = createMPtagRangeAllocator(MPTAG_THORGLOBAL_BASE,MPTAG_THORGLOBAL_COUNT);
    return true;
}

MODULE_EXIT()
{
    ::Release(clusterGroup);
    ::Release(slaveGroup);
    ::Release(dfsGroup);
    ::Release(clusterComm);
    ::Release(ClusterMPAllocator);
}


ThreadAction::ThreadAction(IActionHandler *_tHandler, const char *name, void *_data) : Thread(name), tHandler(_tHandler), data(_data), running(true)
{
}

ThreadAction::~ThreadAction()
{
}

int ThreadAction::run()
{
    while (running)
        if (!tHandler->action(data)) break;
    return 1;
}

void ThreadAction::stop()
{
    running = false;
    join();
}

ThreadAction *createThreadAction(IActionHandler *handler, const char *name, void *data)
{
    ThreadAction *ta = new ThreadAction(handler, name, data);
    ta->start();
    return ta;
}

void destroyThorRowset(PointerArray & data)
{
    ForEachItemIn(idx, data)
        destroyThorRow(data.item(idx));
    data.kill();
}

void * cloneThorRow(size32_t size, const void * ptr)
{
    void * mem = createThorRow(size);
    memcpy(mem, ptr, size);
    return mem;
}

#define EXTRAS 1024
#define NL 3
StringBuffer &ActPrintLogArgsPrep(StringBuffer &res, const CGraphElementBase *container, const ActLogEnum flags, const char *format, va_list args)
{
    if (format)
        res.valist_appendf(format, args).append(" - ");
    res.appendf("activity(%s, %"ACTPF"d)",activityKindStr(container->getKind()), container->queryId());
    if (0 != (flags & thorlog_ecl))
    {
        StringBuffer ecltext;
        container->getEclText(ecltext);
        ecltext.trim();
        if (ecltext.length() > 0)
            res.append(" [ecl=").append(ecltext.str()).append(']');
    }
#ifdef _WIN32
#ifdef MEMLOG
    MEMORYSTATUS mS;
    GlobalMemoryStatus(&mS);
    res.appendf(", mem=%ld",mS.dwAvailPhys);
#endif
#endif
    return res;
}

void ActPrintLogArgs(const CGraphElementBase *container, const ActLogEnum flags, const LogMsgCategory &logCat, const char *format, va_list args)
{
    if ((0 == (flags & thorlog_all)) && (NULL != container->queryOwner().queryOwner() && !container->queryOwner().isGlobal()))
        return; // suppress logging child activities unless thorlog_all flag
    StringBuffer res;
    ActPrintLogArgsPrep(res, container, flags, format, args);
    LOG(logCat, thorJob, "%s", res.str());
}

void ActPrintLogArgs(const CGraphElementBase *container, IException *e, const ActLogEnum flags, const LogMsgCategory &logCat, const char *format, va_list args)
{
    StringBuffer res;
    ActPrintLogArgsPrep(res, container, flags, format, args);
    if (e)
    {
        res.append(" : ");
        e->errorMessage(res);
    }
    LOG(logCat, thorJob, "%s", res.str());
}

void ActPrintLogEx(const CGraphElementBase *container, const ActLogEnum flags, const LogMsgCategory &logCat, const char *format, ...)
{
    if ((0 == (flags & thorlog_all)) && (NULL != container->queryOwner().queryOwner() && !container->queryOwner().isGlobal()))
        return; // suppress logging child activities unless thorlog_all flag
    StringBuffer res;
    va_list args;
    va_start(args, format);
    ActPrintLogArgsPrep(res, container, flags, format, args);
    va_end(args);
    LOG(logCat, thorJob, "%s", res.str());
}

void ActPrintLog(const CActivityBase *activity, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    ActPrintLogArgs(&activity->queryContainer(), thorlog_null, MCdebugProgress, format, args);
    va_end(args);
}

void ActPrintLog(const CActivityBase *activity, IException *e, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    ActPrintLogArgs(&activity->queryContainer(), e, thorlog_null, MCexception(e, MSGCLS_error), format, args);
    va_end(args);
}

void GraphPrintLogArgsPrep(StringBuffer &res, CGraphBase *graph, const ActLogEnum flags, const LogMsgCategory &logCat, const char *format, va_list args)
{
    if (format)
        res.valist_appendf(format, args).append(" - ");
    res.appendf("graph(%s, %"GIDPF"d)", graph->queryJob().queryGraphName(), graph->queryGraphId());
}

void GraphPrintLogArgs(CGraphBase *graph, const ActLogEnum flags, const LogMsgCategory &logCat, const char *format, va_list args)
{
    if ((0 == (flags & thorlog_all)) && (NULL != graph->queryOwner() && !graph->isGlobal()))
        return; // suppress logging from child graph unless thorlog_all flag
    StringBuffer res;
    GraphPrintLogArgsPrep(res, graph, flags, logCat, format, args);
    LOG(logCat, thorJob, "%s", res.str());
}

void GraphPrintLogArgs(CGraphBase *graph, IException *e, const ActLogEnum flags, const LogMsgCategory &logCat, const char *format, va_list args)
{
    if ((0 == (flags & thorlog_all)) && (NULL != graph->queryOwner() && !graph->isGlobal()))
        return; // suppress logging from child graph unless thorlog_all flag
    StringBuffer res;
    GraphPrintLogArgsPrep(res, graph, flags, logCat, format, args);
    if (e)
    {
        res.append(" : ");
        e->errorMessage(res);
    }
    LOG(logCat, thorJob, "%s", res.str());
}

void GraphPrintLog(CGraphBase *graph, IException *e, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    GraphPrintLogArgs(graph, e, thorlog_null, MCexception(e, MSGCLS_error), format, args);
    va_end(args);
}

class CThorException : public CSimpleInterface, implements IThorException
{
protected:
    ThorExceptionAction action;
    ThorActivityKind kind;
    activity_id id;
    graph_id graphId;
    StringAttr jobId;
    int errorcode;
    StringAttr msg;
    LogMsgAudience audience;
    unsigned node;
    MemoryBuffer data; // extra exception specific data
    bool notified;
    unsigned line, column;
    StringAttr file, origin;
    WUExceptionSeverity severity;
public:
    IMPLEMENT_IINTERFACE_USING(CSimpleInterface);
    CThorException(LogMsgAudience _audience,int code, const char *str) 
        : audience(_audience), errorcode(code), msg(str), action(tea_null), graphId(0), id(0), node(0), line(0), column(0), severity(ExceptionSeverityInformation), kind(TAKnone), notified(false) { };
    CThorException(MemoryBuffer &mb)
    {
        mb.read((unsigned &)action);
        mb.read(jobId);
        mb.read(graphId);
        mb.read((unsigned &)kind);
        mb.read(id);
        mb.read((unsigned &)audience);
        mb.read(errorcode);
        mb.read(msg);
        mb.read(file);
        mb.read(line);
        mb.read(column);
        mb.read((int &)severity);
        mb.read(origin);
        size32_t sz;
        mb.read(sz);
        if (sz)
            data.append(sz, mb.readDirect(sz));
    }

// IThorException
    ThorExceptionAction queryAction() { return action; }
    ThorActivityKind queryActivityKind() { return kind; }
    activity_id queryActivityId() { return id; }
    graph_id queryGraphId() { return graphId; }
    const char *queryJobId() { return jobId; }
    void getAssert(StringAttr &_file, unsigned &_line, unsigned &_column) { _file.set(file); _line = line; _column = column; }
    const char *queryOrigin() { return origin; }
    const char *queryMessage() { return msg; }
    WUExceptionSeverity querySeverity() { return severity; }
    const bool queryNotified() const { return notified; }
    MemoryBuffer &queryData() { return data; }
    void setNotified() { notified = true; }
    void setActivityId(activity_id _id) { id = _id; }
    void setActivityKind(ThorActivityKind _kind) { kind = _kind; }
    void setGraphId(graph_id _graphId) { graphId = _graphId; }
    void setJobId(const char *_jobId) { jobId.set(_jobId); }
    void setAction(ThorExceptionAction _action) { action = _action; }
    void setAudience(MessageAudience _audience) { audience = _audience; }
    void setSlave(unsigned _node) { node = _node; }
    void setMessage(const char *_msg) { msg.set(_msg); }
    void setAssert(const char *_file, unsigned _line, unsigned _column) { file.set(_file); line = _line; column = _column; }
    void setOrigin(const char *_origin) { origin.set(_origin); }
    void setSeverity(WUExceptionSeverity _severity) { severity = _severity; }

// IException
    int errorCode() const { return errorcode; }
    StringBuffer &errorMessage(StringBuffer &str) const
    {
        if (!origin.length() || 0 != stricmp("user", origin.get())) // don't report slave in user message
        {
            if (graphId)
                str.append("Graph[").append(graphId).append("], ");
            if (kind)
                str.append(activityKindStr(kind));
            if (id)
            {
                if (kind) str.append('[');
                str.append(id);
                if (kind) str.append(']');
                str.append(": ");
            }
            if (node)
            {
                str.append("SLAVE ");
                queryClusterGroup().queryNode(node).endpoint().getUrlStr(str);
                str.append(": ");
            }
        }
        str.append(msg);
        return str;
    }
    MessageAudience errorAudience() const { return audience; }
};

CThorException *_MakeThorException(LogMsgAudience audience,int code, const char *format, va_list args)
{
    StringBuffer eStr;
    eStr.limited_valist_appendf(1024, format, args);
    return new CThorException(audience, code, eStr.str());
}

CThorException *_ThorWrapException(IException *e, const char *format, va_list args)
{
    StringBuffer eStr;
    eStr.appendf("%d, ", e->errorCode());
    e->errorMessage(eStr).append(" : ");
    eStr.limited_valist_appendf(2048, format, args);
    CThorException *te = new CThorException(e->errorAudience(), e->errorCode(), eStr.toCharArray());
    return te;
}

// convert exception (if necessary) to an exception with action=shutdown
IThorException *MakeThorFatal(IException *e, int code, const char *format, ...)
{
    CThorException *te = QUERYINTERFACE(e, CThorException);
    va_list args;
    va_start(args, format);
    if (te)
        te->Link();
    else
    {
        va_list args;
        va_start(args, format);
        if (e) te = _ThorWrapException(e, format, args);
        else te = _MakeThorException(MSGAUD_user,code, format, args);
        va_end(args);
    }
    te->setAction(tea_shutdown);
    return te;
}

IThorException *MakeThorAudienceException(LogMsgAudience audience, int code, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    IThorException *e = _MakeThorException(audience, code, format, args);
    va_end(args);
    return e;
}

IThorException *MakeThorOperatorException(int code, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    IThorException *e = _MakeThorException(MSGAUD_operator,code, format, args);
    va_end(args);
    return e;
}

void setExceptionActivityInfo(CGraphElementBase &container, IThorException *e)
{
    e->setActivityKind(container.getKind());
    e->setActivityId(container.queryId());
    e->setGraphId(container.queryOwner().queryGraphId());
}

IThorException *_MakeActivityException(CGraphElementBase &container, int code, const char *format, va_list args)
{
    IThorException *e = _MakeThorException(MSGAUD_user, code, format, args);
    setExceptionActivityInfo(container, e);
    return e;
}

IThorException *_MakeActivityException(CGraphElementBase &container, IException *e, const char *_format, va_list args)
{
    StringBuffer format;
    e->errorMessage(format);
    if (_format)
        format.append(", ").append(_format);
    _format = format.str();
    IThorException *e2 = _MakeThorException(e->errorAudience(), e->errorCode(), format.str(), args);
    setExceptionActivityInfo(container, e2);
    return e2;
}

IThorException *MakeActivityException(CActivityBase *activity, int code, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    IThorException *e = _MakeActivityException(activity->queryContainer(), code, format, args);
    va_end(args);
    return e;
}

IThorException *MakeActivityException(CActivityBase *activity, IException *e, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    IThorException *e2 = _MakeActivityException(activity->queryContainer(), e, format, args);
    va_end(args);
    return e2;
}

IThorException *MakeActivityWarning(CActivityBase *activity, int code, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    IThorException *e = _MakeActivityException(activity->queryContainer(), code, format, args);
    e->setAction(tea_warning);
    e->setSeverity(ExceptionSeverityWarning);
    va_end(args);
    return e;
}

IThorException *MakeActivityWarning(CActivityBase *activity, IException *e, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    IThorException *e2 = _MakeActivityException(activity->queryContainer(), e, format, args);
    e2->setAction(tea_warning);
    e2->setSeverity(ExceptionSeverityWarning);
    va_end(args);
    return e2;
}

IThorException *MakeActivityException(CGraphElementBase *container, int code, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    IThorException *e = _MakeActivityException(*container, code, format, args);
    va_end(args);
    return e;
}

IThorException *MakeActivityException(CGraphElementBase *container, IException *e, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    IThorException *e2 = _MakeActivityException(*container, e, format, args);
    va_end(args);
    return e2;
}

IThorException *MakeActivityWarning(CGraphElementBase *container, int code, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    IThorException *e = _MakeActivityException(*container, code, format, args);
    e->setAction(tea_warning);
    e->setSeverity(ExceptionSeverityWarning);
    va_end(args);
    return e;
}

IThorException *MakeActivityWarning(CGraphElementBase *container, IException *e, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    IThorException *e2 = _MakeActivityException(*container, e, format, args);
    e2->setAction(tea_warning);
    e2->setSeverity(ExceptionSeverityWarning);
    va_end(args);
    return e2;
}

IThorException *MakeThorException(int code, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    IThorException *e2 = _MakeThorException(MSGAUD_user,code, format, args);
    va_end(args);
    return e2;
}

IThorException *MakeThorException(IException *e)
{
    IThorException *te = QUERYINTERFACE(e, IThorException);
    if (te)
        return LINK(te);
    StringBuffer msg;
    return new CThorException(MSGAUD_user, e->errorCode(), e->errorMessage(msg).str());
}

IThorException *ThorWrapException(IException *e, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    ThorExceptionAction action=tea_null;
    if (QUERYINTERFACE(e, ISEH_Exception))
        action = tea_shutdown;
    CThorException *te = _ThorWrapException(e, format, args);
    te->setAction(action);
    va_end(args);
    return te;
}

StringBuffer &getLogDir(const char *prefix, const char *logdir, StringBuffer &logname) 
{
    if (logdir && *logdir !='\0' && recursiveCreateDirectory(logdir))
        logname.append(logdir);
    else
    {
        LOG(MCwarning, thorJob, "Missing or invalid log directory - using current working directory");
        char cwd[1024];
        GetCurrentDirectory(1024, cwd);
        logname.append(cwd);
    }

    if (logname.length() && logname.charAt(logname.length()-1) != PATHSEPCHAR)
        logname.append(PATHSEPCHAR);
    logname.append(prefix);
    logname.append(".log");
    return logname;
}

#if 0
void SetLogName(const char *prefix, const char *logdir, const char *thorname, bool master) 
{
    StringBuffer logname;
    if (logdir && *logdir !='\0')
    {
        if (!recursiveCreateDirectory(logdir))
        {
            PrintLog("Failed to use %s as log directory, using current working directory", logdir); // default working directory should be open already
            return;
        }
        logname.append(logdir);
    }
    else
    {
        char cwd[1024];
        GetCurrentDirectory(1024, cwd);
        logname.append(cwd);
    }

    if (logname.length() && logname.charAt(logname.length()-1) != PATHSEPCHAR)
        logname.append(PATHSEPCHAR);
    logname.append(prefix);
#if 0
    time_t tNow;
    time(&tNow);
    char timeStamp[32];
#ifdef _WIN32
    struct tm *ltNow;
    ltNow = localtime(&tNow);
    strftime(timeStamp, 32, ".%m_%d_%y_%H_%M_%S", ltNow);
#else
    struct tm ltNow;
    localtime_r(&tNow, &ltNow);
    strftime(timeStamp, 32, ".%m_%d_%y_%H_%M_%S", &ltNow);
#endif
    logname.append(timeStamp);
#endif
    logname.append(".log");
    openLogFile(logname.toCharArray());
    PrintLog("Opened log file %s", logname.toCharArray());
    PrintLog("Build %s", BUILD_TAG);
}
#endif

#ifdef __linux__
int file_select(const struct dirent *entry)
{
    return strncmp(entry->d_name, "thtmp", 5) == 0;
}
#endif

class CTempNameHandler
{
public:
    unsigned num;
    StringAttr tempdir;
    StringAttr alttempdir; // only set if needed
    CriticalSection crit;
    bool altallowed;
    bool cleardir;

    CTempNameHandler()
    {
        num = 0;
        altallowed = false;
        cleardir = false;
    }

    ~CTempNameHandler()
    {
        if (cleardir) 
            clearDirs(false);       // don't log as jlog may have closed
    }


    const char *queryTempDir(bool alt) 
    { 
        if (alt&&altallowed) 
            return alttempdir;
        return tempdir; 
    }

    void setTempDir(const char *name,bool clear)
    {
        CriticalBlock block(crit);
        assertex(tempdir.isEmpty()); // should only be called once
        StringBuffer base;
        if (name&&*name) {
            base.append(name);
            addPathSepChar(base);
        }
        else
        {
#ifdef _WIN32
            base.append("c:\\thortemp");
#else
            base.append("/c$/thortemp");
#endif
            base.append("_").append(globals->queryProp("@name"));
            addPathSepChar(base);
        }
        tempdir.set(base.toCharArray());
        recursiveCreateDirectory(tempdir);
#ifdef _WIN32
        altallowed = false;
#else
        altallowed = globals->getPropBool("@thor_dual_drive",true);
#endif
        if (altallowed) {
            unsigned d = getPathDrive(tempdir);
            if (d>1)
                altallowed = false;
            else {
                StringBuffer p(tempdir);
                alttempdir.set(setPathDrive(p,d?0:1).str());
                recursiveCreateDirectory(alttempdir);
            }
        }
        cleardir = clear;
        if (clear)
            clearDirs(true);
    }

    static void clearDir(const char *dir, bool log)
    {
        if (dir&&*dir) {
            Owned<IDirectoryIterator> iter = createDirectoryIterator(dir);
            ForEach (*iter)
            {
                IFile &file = iter->query();
                if (file.isFile())
                {
                    if (log)
                        LOG(MCdebugInfo, thorJob, "Deleting %s", file.queryFilename());
                    try { file.remove(); }
                    catch (IException *e)
                    {
                        if (log)
                            FLLOG(MCwarning, thorJob, e);
                        e->Release();
                    }
                }
            }
        }
    }

    void clearDirs(bool log)
    {
        clearDir(tempdir,log);
        clearDir(alttempdir,log);
    }


    void getTempName(StringBuffer &name, const char *suffix,bool alt)
    {
        CriticalBlock block(crit);
        assertex(!tempdir.isEmpty()); // should only be called once
        if (alt && altallowed)
            name.append(alttempdir);
        else
            name.append(tempdir);
        name.append("thtmp").append((unsigned)GetCurrentProcessId()).append('_').append(++num);
        if (suffix)
            name.append("__").append(suffix);
        name.append(".tmp");
    }


} TempNameHandler;



void GetTempName(StringBuffer &name, const char *prefix,bool altdisk)
{
    TempNameHandler.getTempName(name, prefix, altdisk);
}

void SetTempDir(const char *name,bool clear)
{
    TempNameHandler.setTempDir(name,clear);
}

void ClearDir(const char *dir)
{
    CTempNameHandler::clearDir(dir,true);
}

void ClearTempDirs()
{
    TempNameHandler.clearDirs(true);
    PROGLOG("temp directory cleared");
}


const char *queryTempDir(bool altdisk)
{
    return TempNameHandler.queryTempDir(altdisk);
}

class CBarrierAbortException: public CSimpleInterface, public IBarrierException
{
public:
    IMPLEMENT_IINTERFACE_USING(CSimpleInterface);
// IThorException
    int errorCode() const { return -1; }
    StringBuffer &errorMessage(StringBuffer &str) const { str.append("Barrier Aborted"); return str; }
    MessageAudience errorAudience() const { return MSGAUD_user; }
};

IBarrierException *createBarrierAbortException()
{
    return new CBarrierAbortException();
}

void loadCmdProp(IPropertyTree *tree, const char *cmdProp)
{
    StringBuffer prop("@"), val;
    while (*cmdProp && *cmdProp != '=')
        prop.append(*cmdProp++);
    if (*cmdProp)
    {
        cmdProp++;
        while (isspace(*cmdProp))
            cmdProp++;
        while (*cmdProp)
            val.append(*cmdProp++);
        prop.clip();
        val.clip();
        if (prop.length())
            tree->setProp(prop.toCharArray(), val.toCharArray());
    }
}

const LogMsgJobInfo thorJob(UnknownJob, UnknownUser); // may be improved later

void ensureDirectoryForFile(const char *fName)
{
    if (!recursiveCreateDirectoryForFile(fName))
        throw MakeOsException(GetLastError(), "Failed to create directory for file: %s", fName);
}

// Not recommended to be used from slaves as tend to be one or more trying at same time.
void reportExceptionToWorkunit(IConstWorkUnit &workunit,IException *e, WUExceptionSeverity severity)
{
    LOG(MCwarning, thorJob, e, "Reporting exception to WU");
    Owned<IWorkUnit> wu = &workunit.lock();
    if (wu)
    {
        Owned<IWUException> we = wu->createException();
        StringBuffer s;
        we->setExceptionMessage(e->errorMessage(s.clear()).str());
        we->setExceptionCode(e->errorCode());
        IThorException *te = QUERYINTERFACE(e, IThorException);
        if (te)
        {
            we->setSeverity(te->querySeverity());
            if (te->queryOrigin())
                we->setExceptionSource(te->queryOrigin());
            StringAttr file;
            unsigned line, column;
            te->getAssert(file, line, column);
            if (file.length())
                we->setExceptionFileName(file);
            if (line || column)
            {
                we->setExceptionLineNo(line);
                we->setExceptionColumn(column);
            }
        }
        else
            we->setSeverity(severity);
    }
} 

static memsize_t largeMemSize = 0;
memsize_t queryLargeMemSize()
{
    if (!largeMemSize)
    {
        if (globals->hasProp("largeMemSize"))
            largeMemSize = globals->getPropInt("@largeMemSize") * 0x100000;
        else
            largeMemSize = DEFAULT_LARGEMEM_BUFFER_SIZE;
        PROGLOG("Setting largemem to %"I64F"d", (unsigned __int64) largeMemSize);
    }
    return largeMemSize;
}

StringBuffer &getCompoundQueryName(StringBuffer &compoundName, const char *queryName, unsigned version)
{
    return compoundName.append('V').append(version).append('_').append(queryName);
}

void setClusterGroup(IGroup *group)
{
    ::Release(clusterComm);
    ::Release(clusterGroup);
    ::Release(slaveGroup);
    ::Release(dfsGroup);
    clusterGroup = LINK(group);
    // slaveGroup contains endpoints with mp ports of slaves
    slaveGroup = clusterGroup->remove(0);
    // dfsGroup will match named group in dfs
    Owned<INodeIterator> nodeIter = slaveGroup->getIterator();
    IArrayOf<INode> nodes;
    ForEach(*nodeIter)
        nodes.append(*createINodeIP(nodeIter->query().endpoint(),0));
    dfsGroup = createIGroup(nodes.ordinality(), nodes.getArray());
    clusterComm = createCommunicator(clusterGroup);
}
const bool clusterInitialized() { return NULL != clusterComm; }
ICommunicator &queryClusterComm() { return *clusterComm; }
IGroup &queryClusterGroup() { return *clusterGroup; }
IGroup &querySlaveGroup() { return *slaveGroup; }
IGroup &queryDfsGroup() { return *dfsGroup; }
unsigned queryClusterWidth() { return clusterGroup->ordinality()-1; }
unsigned queryClusterNode() { return clusterGroup->rank(queryMyNode()); };


mptag_t allocateClusterMPTag()
{
    return ClusterMPAllocator->alloc();
}

void freeClusterMPTag(mptag_t tag)
{
    ClusterMPAllocator->release(tag);
}

IThorException *deserializeThorException(MemoryBuffer &in)
{
    unsigned te;
    in.read(te);
    if (!te)
    {
        Owned<IException> e = deserializeException(in);
        StringBuffer s;
        return new CThorException(e->errorAudience(), e->errorCode(), e->errorMessage(s).str());
    }
    return new CThorException(in);
}

void serializeThorException(IException *e, MemoryBuffer &out)
{
    IThorException *te = QUERYINTERFACE(e, IThorException);
    if (!te)
    {
        out.append(0);
        serializeException(e, out);
        return;
    }
    out.append(1);
    out.append((unsigned)te->queryAction());
    out.append(te->queryJobId());
    out.append(te->queryGraphId());
    out.append((unsigned)te->queryActivityKind());
    out.append(te->queryActivityId());
    out.append((unsigned)te->errorAudience());
    out.append(te->errorCode());
    out.append(te->queryMessage());
    StringAttr file;
    unsigned line, column;
    te->getAssert(file, line, column);
    out.append(file);
    out.append(line);
    out.append(column);
    out.append(te->querySeverity());
    out.append(te->queryOrigin());
    MemoryBuffer &data = te->queryData();
    out.append((size32_t)data.length());
    if (data.length())
        out.append(data.length(), data.toByteArray());
}

bool getBestFilePart(CActivityBase *activity, IPartDescriptor &partDesc, OwnedIFile & ifile, unsigned &location, StringBuffer &path, IExceptionHandler *eHandler)
{
    if (0 == partDesc.numCopies()) // not sure this is poss.
        return false;
    SocketEndpoint slfEp((unsigned short)0);
    unsigned l;

    RemoteFilename rfn;
    StringBuffer locationName, primaryName;
    //First check for local matches
    for (l=0; l<partDesc.numCopies(); l++)
    {
        rfn.clear();
        partDesc.getFilename(l, rfn);
        if (0 == l)
        {
            rfn.getPath(locationName.clear());
            assertex(locationName.length());
            primaryName.append(locationName);
            locationName.clear();
        }
        if (rfn.isLocal())
        {
            rfn.getPath(locationName.clear());
            assertex(locationName.length());
            Owned<IFile> file = createIFile(locationName.str());
            try
            {
                if (file->exists())
                {
                    ifile.set(file);
                    location = l;
                    path.append(locationName);
                    return true;
                }
            }
            catch (IException *e)
            {
                ActPrintLog(&activity->queryContainer(), e, "getBestFilePart");
                e->Release();
            }
        }
    }

    //Now check for a remote match...
    for (l=0; l<partDesc.numCopies(); l++)
    {
        rfn.clear();
        partDesc.getFilename(l, rfn);
        if (!rfn.isLocal())
        {
            rfn.getPath(locationName.clear());
            assertex(locationName.length());
            Owned<IFile> file = createIFile(locationName.str());
            try
            {
                if (file->exists())
                {
                    ifile.set(file);
                    location = l;
                    if (0 != l)
                    {
                        IThorException *e = MakeActivityWarning(activity, 0, "Primary file missing: %s, using remote copy: %s", primaryName.str(), locationName.str());
                        if (!eHandler)
                            throw e;
                        eHandler->fireException(e);
                    }
                    path.append(locationName);
                    return true;
                }
            }
            catch (IException *e)
            {
                ActPrintLog(&activity->queryContainer(), e, NULL);
                e->Release();
            }
        }
    }
    return false;
}

StringBuffer &getFilePartLocations(IPartDescriptor &partDesc, StringBuffer &locations)
{
    unsigned l;
    for (l=0; l<partDesc.numCopies(); l++)
    {
        RemoteFilename rfn;
        partDesc.getFilename(l, rfn);
        rfn.getRemotePath(locations);
        if (l != partDesc.numCopies()-1)
            locations.append(", ");
    }
    return locations;
}

StringBuffer &getPartFilename(IPartDescriptor &partDesc, unsigned copy, StringBuffer &filePath, bool localMount)
{
    RemoteFilename rfn;
    if (localMount && copy)
    {
        partDesc.getFilename(0, rfn);
        if (!rfn.isLocal())
            localMount = false;
        rfn.clear();
    }
    partDesc.getFilename(copy, rfn);
    rfn.getPath(filePath);
    return filePath;
}

// CFifoFileCache impl.

void CFifoFileCache::deleteFile(IFile &ifile)
{
    try 
    {
        if (!ifile.remove())
            FLLOG(MCoperatorWarning, thorJob, "CFifoFileCache: Failed to remove file (missing) : %s", ifile.queryFilename());
    }
    catch (IException *e)
    {
        StringBuffer s("Failed to remove file: ");
        FLLOG(MCoperatorWarning, thorJob, e, s.append(ifile.queryFilename()));
    }
}

void CFifoFileCache::init(const char *cacheDir, unsigned _limit, const char *pattern)
{
    limit = _limit;
    Owned<IDirectoryIterator> iter = createDirectoryIterator(cacheDir, pattern);
    ForEach (*iter)
    {
        IFile &file = iter->query();
        if (file.isFile())
            deleteFile(file);
    }
}

void CFifoFileCache::add(const char *filename)
{
    unsigned pos = files.find(filename);
    if (NotFound != pos)
        files.remove(pos);
    files.add(filename, 0);
    if (files.ordinality() > limit)
    {
        const char *toRemoveFname = files.item(limit);
        PROGLOG("Removing %s from fifo cache", toRemoveFname);
        OwnedIFile ifile = createIFile(toRemoveFname);
        deleteFile(*ifile);
        files.remove(limit);
    }
}

bool CFifoFileCache::isAvailable(const char *filename)
{
    unsigned pos = files.find(filename);
    if (NotFound != pos)
    {
        OwnedIFile ifile = createIFile(filename);
        if (ifile->exists())
            return true;
    }
    return false;
}

IOutputMetaData *createFixedSizeMetaData(size32_t sz)
{
    // sure if this allowed or is cheating!
    return new thormisc::CFixedOutputMetaData(sz);
}


class CRowStreamFromNode : public CSimpleInterface, implements IRowStream
{
    CActivityBase &activity;
    unsigned node, myNode;
    ICommunicator &comm;
    MemoryBuffer mb;
    bool eos;
    const bool &abortSoon;
    mptag_t mpTag;
    Owned<ISerialStream> bufferStream;
    CThorStreamDeserializerSource memDeserializer;

public:
    IMPLEMENT_IINTERFACE_USING(CSimpleInterface);

    CRowStreamFromNode(CActivityBase &_activity, unsigned _node, ICommunicator &_comm, mptag_t _mpTag, const bool &_abortSoon) : activity(_activity), node(_node), comm(_comm), mpTag(_mpTag), abortSoon(_abortSoon)
    {
        bufferStream.setown(createMemoryBufferSerialStream(mb));
        memDeserializer.setStream(bufferStream);
        myNode = comm.queryGroup().rank(queryMyNode());
        eos = false;
    }
// IRowStream
    const void *nextRow()
    {
        if (eos) return NULL;

        loop
        {
            while (!memDeserializer.eos()) 
            {
                RtlDynamicRowBuilder rowBuilder(activity.queryRowAllocator());
                size32_t sz = activity.queryRowDeserializer()->deserialize(rowBuilder, memDeserializer);
                return rowBuilder.finalizeRowClear(sz);
            }
            CMessageBuffer msg; // no msg just give me data
            if (!comm.send(msg, node, mpTag, LONGTIMEOUT)) // should never timeout, unless other end down
                throw MakeStringException(0, "CRowStreamFromNode: Failed to send data request from node %d, to node %d", myNode, node);
            loop
            {
                if (abortSoon)
                    break;
                if (comm.recv(msg, node, mpTag, NULL, 60000))
                    break;
                ActPrintLog(&activity, "CRowStreamFromNode, request more from node %d, tag %d timedout, retrying", node, mpTag);
            }
            if (!msg.length())
                break;
            if (abortSoon)
                break;
            msg.swapWith(mb);
        }
        eos = true;
        return NULL;
    }
    void stop()
    {
        CMessageBuffer msg;
        msg.append(1); // stop
        verifyex(comm.send(msg, node, mpTag));
    }
};

IRowStream *createRowStreamFromNode(CActivityBase &activity, unsigned node, ICommunicator &comm, mptag_t mpTag, const bool &abortSoon)
{
    return new CRowStreamFromNode(activity, node, comm, mpTag, abortSoon);
}

#define DEFAULT_ROWSERVER_BUFF_SIZE                 (0x10000)               // 64K
class CRowServer : public CSimpleInterface, implements IThreaded, implements IRowServer
{
    CThreaded threaded;
    ICommunicator &comm;
    CActivityBase *activity;
    mptag_t mpTag;
    unsigned myNode, fetchBuffSize;
    Linked<IRowStream> seq;
    bool running;

public:
    IMPLEMENT_IINTERFACE_USING(CSimpleInterface);

    CRowServer(CActivityBase *_activity, IRowStream *_seq, ICommunicator &_comm, mptag_t _mpTag) 
        : activity(_activity), seq(_seq), comm(_comm), mpTag(_mpTag), threaded("CRowServer")
    {
        fetchBuffSize = DEFAULT_ROWSERVER_BUFF_SIZE;
        running = true;
        threaded.init(this);
    }
    ~CRowServer()
    {
        stop();
        threaded.join();
    }
    virtual void main()
    {
        CMessageBuffer mb;
        while (running)
        {
            rank_t sender;
            if (comm.recv(mb, RANK_ALL, mpTag, &sender))
            {
                unsigned code;
                if (mb.length())
                {
                    mb.read(code);
                    if (1 == code) // stop
                    {
                        seq->stop();
                        break;
                    }
                    else
                        throwUnexpected();
                }
                mb.clear();
                CMemoryRowSerializer mbs(mb);
                do
                {
                    OwnedConstThorRow row = seq->nextRow();
                    if (!row)
                        break;
                    activity->queryRowSerializer()->serialize(mbs,(const byte *)row.get());

                } while (mb.length() < fetchBuffSize); // NB: allows at least 1
                if (!comm.send(mb, sender, mpTag, LONGTIMEOUT))
                    throw MakeStringException(0, "CRowStreamFromNode: Failed to send data back to node: %d", activity->queryContainer().queryJob().queryMyRank());
                mb.clear();
            }
        }
        running = false;
    }
    void stop() { running = false; comm.cancel(RANK_ALL, mpTag); }
};

IRowServer *createRowServer(CActivityBase *activity, IRowStream *seq, ICommunicator &comm, mptag_t mpTag)
{
    return new CRowServer(activity, seq, comm, mpTag);
}

IRowStream *createUngroupStream(IRowStream *input)
{
    class CUngroupStream : public CSimpleInterface, implements IRowStream
    {
        IRowStream *input;
    public:
        CUngroupStream(IRowStream *_input) : input(_input) { input->Link(); }
        ~CUngroupStream() { input->Release(); }
        IMPLEMENT_IINTERFACE_USING(CSimpleInterface);
        virtual const void *nextRow() 
        {
            const void *ret = input->nextRow(); 
            if (ret) 
                return ret;
            else
                return input->nextRow();
        }
        virtual void stop()
        {
            input->stop();
        }
    };
    return new CUngroupStream(input);
}
