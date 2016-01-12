//---------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
//----------------------------------------------------------------------------

#include "JsrtPch.h"
#include "JsrtDebug.h"
#include "JsrtDebugEventObject.h"
#include "JsrtDebugUtils.h"
#include "..\Runtime\Debug\RuntimeDebugPch.h"
#include "screrror.h"   // For CompileScriptException

JsrtDebug::JsrtDebug(ThreadContext* threadContext) :
    threadContext(threadContext),
    debugEventCallback(nullptr),
    callbackState(nullptr),
    resumeAction(BREAKRESUMEACTION_CONTINUE),
    debugObjectArena(nullptr),
    debuggerObjectsManager(nullptr),
    callBackDepth(0),
    debugDocumentManager(nullptr),
    breakOnExceptionType(JsDiagBreakOnExceptionTypeUncaught)
{
    // ToDo (SaAgarwa): Confirm the default value of breakOnExceptionType
    Assert(threadContext != nullptr);
}

JsrtDebug::~JsrtDebug()
{
    this->threadContext = nullptr;
    this->debugEventCallback = nullptr;
    this->callbackState = nullptr;

    if (this->debuggerObjectsManager != nullptr)
    {
        Adelete(this->debugObjectArena, this->debuggerObjectsManager);
        this->debuggerObjectsManager = nullptr;
    }

    if (this->debugDocumentManager != nullptr)
    {
        Adelete(this->debugObjectArena, this->debugDocumentManager);
        this->debugDocumentManager = nullptr;
    }

    if (this->debugObjectArena != nullptr)
    {
        this->threadContext->GetRecycler()->UnregisterExternalGuestArena(this->debugObjectArena);
        HeapDelete(this->debugObjectArena);
        this->debugObjectArena = nullptr;
    }
}

void JsrtDebug::SetDebugEventCallback(JsDiagDebugEventCallback debugEventCallback, void* callbackState)
{
    this->debugEventCallback = debugEventCallback;
    this->callbackState = callbackState;
}

bool JsrtDebug::CanHalt(Js::InterpreterHaltState* haltState)
{
    // This is registered as the callback for inline breakpoints.
    // We decide here if we are at a reasonable stop location that has source code.
    Assert(haltState->IsValid());

    Js::FunctionBody* pCurrentFuncBody = haltState->GetFunction();
    int byteOffset = haltState->GetCurrentOffset();
    Js::FunctionBody::StatementMap* map = pCurrentFuncBody->GetMatchingStatementMapFromByteCode(byteOffset, false);

    // Resolve the dummy ret code.
    return map != nullptr && (!pCurrentFuncBody->GetIsGlobalFunc() || !Js::FunctionBody::IsDummyGlobalRetStatement(&map->sourceSpan));
}

void JsrtDebug::DispatchHalt(Js::InterpreterHaltState* haltState)
{
    switch (haltState->stopType)
    {
    case Js::STOP_BREAKPOINT:
    case Js::STOP_INLINEBREAKPOINT:
    case Js::STOP_ASYNCBREAK:
        this->ReportBreak(haltState);
        break;
    case Js::STOP_STEPCOMPLETE:
        this->SetResumeType(BREAKRESUMEACTION_CONTINUE);
        this->ReportBreak(haltState);
        break;
    case Js::STOP_EXCEPTIONTHROW:
        this->ReportExceptionBreak(haltState);
        break;
    case Js::STOP_MUTATIONBREAKPOINT:
        AssertMsg(false, "Not yet handled");
        break;
    default:
        AssertMsg(false, "Unhandled stop type");
    }

    this->HandleResume(haltState, this->resumeAction);
}

bool JsrtDebug::CanAllowBreakpoints()
{
    return true;
}

void JsrtDebug::CleanupHalt()
{
}

bool JsrtDebug::IsInClosedState()
{
    return this->debugEventCallback == nullptr;
}

bool JsrtDebug::IsExceptionReportingEnabled()
{
    return this->breakOnExceptionType != JsDiagBreakOnExceptionTypeNone;
}

bool JsrtDebug::IsFirstChanceExceptionEnabled()
{
    return this->breakOnExceptionType == JsDiagBreakOnExceptionTypeAll;
}

void JsrtDebug::ReportScriptCompile(Js::JavascriptFunction * scriptFunction, Js::Utf8SourceInfo* utf8SourceInfo, CompileScriptException* compileException)
{
    if (this->debugEventCallback != nullptr)
    {
        Js::ScriptContext* scriptContext = utf8SourceInfo->GetScriptContext();

        JsrtDebugEventObject debugEventObject(scriptContext);

        JsrtDebugUtils::AddSouceIdToObject((Js::DynamicObject *)debugEventObject.GetEventDataObject(), utf8SourceInfo);
        JsrtDebugUtils::AddSouceUrlToObject((Js::DynamicObject *)debugEventObject.GetEventDataObject(), utf8SourceInfo);

        JsDiagDebugEvent jsDiagDebugEvent = JsDiagDebugEventCompileError;

        if (scriptFunction == nullptr)
        {
            // Report JsDiagDebugEventCompileError event
            JsrtDebugUtils::AddErrorToObject((Js::DynamicObject *)debugEventObject.GetEventDataObject(), scriptContext, compileException->ei.bstrDescription);
        }
        else
        {
            // Create DebugDocument and then report JsDiagDebugEventSourceCompilation event
            Js::DebugDocument* debugDocument = HeapNewNoThrow(Js::DebugDocument, utf8SourceInfo, scriptFunction->GetFunctionBody());
            if (debugDocument != nullptr)
            {
                utf8SourceInfo->SetDebugDocument(debugDocument);
            }
            jsDiagDebugEvent = JsDiagDebugEventSourceCompilation;
        }

        this->CallDebugEventCallback(jsDiagDebugEvent, debugEventObject.GetEventDataObject());
    }
}

void JsrtDebug::ReportBreak(Js::InterpreterHaltState * haltState)
{
    if (this->debugEventCallback != nullptr)
    {
        Js::Utf8SourceInfo* utf8SourceInfo = haltState->GetFunction()->GetUtf8SourceInfo();
        Js::ScriptContext* scriptContext = utf8SourceInfo->GetScriptContext();

        JsDiagDebugEvent jsDiagDebugEvent = haltState->stopType == Js::STOP_ASYNCBREAK ? JsDiagDebugEventAsyncBreak : JsDiagDebugEventBreak;

        /*{"sourceId":sourceContext,"sourceUrl":sourceUrl,"line":lineNumber,"column":sourceColumn,"sourceText":sourceLineofBreak,"breakId":breakpointId}*/

        JsrtDebugEventObject debugEventObject(scriptContext);

        JsrtDebugUtils::AddSouceIdToObject((Js::DynamicObject *)debugEventObject.GetEventDataObject(), utf8SourceInfo);
        JsrtDebugUtils::AddSouceUrlToObject((Js::DynamicObject *)debugEventObject.GetEventDataObject(), utf8SourceInfo);

        Js::FunctionBody* functionBody = haltState->topFrame->GetFunction();

        if (functionBody != nullptr)
        {
            int currentByteCodeOffset = haltState->topFrame->GetByteCodeOffset();

            JsrtDebugUtils::AddLineColumnToObject((Js::DynamicObject *)debugEventObject.GetEventDataObject(), functionBody, currentByteCodeOffset);
            JsrtDebugUtils::AddSourceTextToObject((Js::DynamicObject *)debugEventObject.GetEventDataObject(), functionBody, currentByteCodeOffset);
        }

        BEGIN_LEAVE_SCRIPT(scriptContext)
        {
            AutoSetDispatchHaltFlag autoSetDispatchHaltFlag(scriptContext, scriptContext->GetThreadContext());
#if DBG
            void *frameAddress = _AddressOfReturnAddress();
            scriptContext->GetThreadContext()->GetDebugManager()->SetDispatchHaltFrameAddress(frameAddress);
#endif
            this->CallDebugEventCallback(jsDiagDebugEvent, debugEventObject.GetEventDataObject());
        }
        END_LEAVE_SCRIPT(scriptContext);
    }
}

void JsrtDebug::ReportExceptionBreak(Js::InterpreterHaltState * haltState)
{
    if (this->debugEventCallback != nullptr)
    {
        Js::Utf8SourceInfo* utf8SourceInfo = haltState->GetFunction()->GetUtf8SourceInfo();
        Js::ScriptContext* scriptContext = utf8SourceInfo->GetScriptContext();

        JsDiagDebugEvent jsDiagDebugEvent = JsDiagDebugEventRuntimeException;

        JsrtDebugEventObject debugEventObject(scriptContext);

        Js::DynamicObject* eventDataObject = debugEventObject.GetEventDataObject();

        JsrtDebugUtils::AddSouceIdToObject(eventDataObject, utf8SourceInfo);
        JsrtDebugUtils::AddSouceUrlToObject(eventDataObject, utf8SourceInfo);

        Js::FunctionBody* functionBody = haltState->topFrame->GetFunction();

        if (functionBody != nullptr)
        {
            int currentByteCodeOffset = haltState->topFrame->GetByteCodeOffset();

            JsrtDebugUtils::AddLineColumnToObject(eventDataObject, functionBody, currentByteCodeOffset);
            JsrtDebugUtils::AddSourceTextToObject(eventDataObject, functionBody, currentByteCodeOffset);
        }

        JsrtDebugUtils::AddBooleanPropertyToObject(eventDataObject, L"uncaught", !haltState->exceptionObject->IsFirstChanceException(), scriptContext);

        // ToDo (SaAgarwa): Refactor ActiveScriptError and resue it here
        if (haltState->exceptionObject != nullptr)
        {
            Js::Var errorObject = haltState->exceptionObject->GetThrownObject(nullptr);
            if (Js::JavascriptError::Is(errorObject) ||
                Js::JavascriptError::IsRemoteError(errorObject))
            {
                wchar_t const * messageSz = nullptr;
                Js::JavascriptError::GetRuntimeError(Js::RecyclableObject::FromVar(errorObject), &messageSz);
                if (messageSz != nullptr)
                {
                    JsrtDebugUtils::AddStringPropertyToObject(eventDataObject, L"errorText", messageSz, scriptContext);
                }
            }
        }

        BEGIN_LEAVE_SCRIPT(scriptContext)
        {
            AutoSetDispatchHaltFlag autoSetDispatchHaltFlag(scriptContext, scriptContext->GetThreadContext());
#if DBG
            void *frameAddress = _AddressOfReturnAddress();
            scriptContext->GetThreadContext()->GetDebugManager()->SetDispatchHaltFrameAddress(frameAddress);
#endif
            this->CallDebugEventCallback(jsDiagDebugEvent, debugEventObject.GetEventDataObject());
        }
        END_LEAVE_SCRIPT(scriptContext);
    }
}

void JsrtDebug::HandleResume(Js::InterpreterHaltState * haltState, BREAKRESUMEACTION resumeAction)
{
    Js::ScriptContext* scriptContext = haltState->framePointers->Peek()->GetScriptContext();

    if (resumeAction == BREAKRESUMEACTION_ABORT)
    {
        // In this case we need to abort script execution entirely and also stop working on any breakpoint for this engine.
        scriptContext->GetDebugContext()->GetProbeContainer()->RemoveAllProbes();
        scriptContext->GetDebugContext()->GetProbeContainer()->UninstallInlineBreakpointProbe(NULL);
        scriptContext->GetDebugContext()->GetProbeContainer()->UninstallDebuggerScriptOptionCallback();
        throw Js::ScriptAbortException();
    }
    else
    {
        scriptContext->GetThreadContext()->GetDebugManager()->stepController.HandleResumeAction(haltState, resumeAction);
    }
}

void JsrtDebug::SetResumeType(BREAKRESUMEACTION resumeAction)
{
    this->resumeAction = resumeAction;
}

bool JsrtDebug::EnableAsyncBreak(Js::ScriptContext* scriptContext)
{
    // This can be called when we are already at break
    if (!scriptContext->GetDebugContext()->GetProbeContainer()->IsAsyncActivate())
    {
        scriptContext->GetDebugContext()->GetProbeContainer()->AsyncActivate(this);
        if (Js::Configuration::Global.EnableJitInDebugMode())
        {
            scriptContext->GetThreadContext()->GetDebugManager()->GetDebuggingFlags()->SetForceInterpreter(true);
        }
        return true;
    }
    return false;
}

void JsrtDebug::CallDebugEventCallback(JsDiagDebugEvent debugEvent, Js::DynamicObject * eventDataObject)
{
    this->callBackDepth++;
    this->debugEventCallback(debugEvent, eventDataObject, this->callbackState);
    this->callBackDepth--;

    if (this->callBackDepth == 0)
    {
        this->GetDebuggerObjectsManager()->ClearAll();
    }
}

Js::DynamicObject * JsrtDebug::GetScript(Js::Utf8SourceInfo * utf8SourceInfo)
{
    DebuggerObjectBase* debuggerObject = DebuggerObjectScript::Make(this->GetDebuggerObjectsManager(), utf8SourceInfo);
    return debuggerObject->GetJSONObject(utf8SourceInfo->GetScriptContext());
}

Js::JavascriptArray * JsrtDebug::GetScripts(Js::ScriptContext* scriptContext)
{
    Js::JavascriptArray* scriptsArray = scriptContext->GetLibrary()->CreateArray();

    for (Js::ScriptContext *tempScriptContext = scriptContext->GetThreadContext()->GetScriptContextList();
    tempScriptContext != nullptr && !tempScriptContext->IsClosed();
        tempScriptContext = tempScriptContext->next)
    {
        int index = 0;
        tempScriptContext->GetSourceList()->Map([&](int i, RecyclerWeakReference<Js::Utf8SourceInfo>* sourceInfoWeakRef)
        {
            Js::Utf8SourceInfo* sourceInfo = sourceInfoWeakRef->Get();
            if (sourceInfo != nullptr)
            {
                // ToDo (SaAgarwa): This can allocate memory and can trigger GC, what if some Utf8SourceInfo gets recycle at that time?
                Js::DynamicObject* sourceObj = this->GetScript(sourceInfo);
                if (sourceObj != nullptr)
                {
                    Js::Var marshaledObj = Js::CrossSite::MarshalVar(scriptContext, sourceObj);
                    Js::JavascriptOperators::OP_SetElementI((Js::Var)scriptsArray, Js::JavascriptNumber::ToVar(index, scriptContext), marshaledObj, scriptContext);
                    index++;
                }
            }
        });
    }

    return scriptsArray;
}

Js::DynamicObject * JsrtDebug::GetSource(uint scriptId)
{
    Js::Utf8SourceInfo* utf8SourceInfo = nullptr;

    for (Js::ScriptContext *scriptContext = this->threadContext->GetScriptContextList();
    scriptContext != nullptr && utf8SourceInfo == nullptr && !scriptContext->IsClosed();
        scriptContext = scriptContext->next)
    {
        scriptContext->GetSourceList()->MapUntil([&](int i, RecyclerWeakReference<Js::Utf8SourceInfo>* sourceInfoWeakRef) -> bool
        {
            Js::Utf8SourceInfo* sourceInfo = sourceInfoWeakRef->Get();
            if (sourceInfo != nullptr && sourceInfo->GetSourceInfoId() == scriptId)
            {
                utf8SourceInfo = sourceInfo;
                return true;
            }
            return false;
        });
    }

    Js::DynamicObject* sourceObject = utf8SourceInfo->GetScriptContext()->GetLibrary()->CreateObject();

    if (utf8SourceInfo != nullptr)
    {
        JsrtDebugUtils::AddSouceIdToObject(sourceObject, utf8SourceInfo);
        JsrtDebugUtils::AddSouceToObject(sourceObject, utf8SourceInfo);
        JsrtDebugUtils::AddDoublePropertyToObject(sourceObject, L"length", utf8SourceInfo->GetCchLength(), utf8SourceInfo->GetScriptContext());
        JsrtDebugUtils::AddDoublePropertyToObject(sourceObject, L"fromLine", 0, utf8SourceInfo->GetScriptContext());
        JsrtDebugUtils::AddDoublePropertyToObject(sourceObject, L"toLine", utf8SourceInfo->GetLineCount(), utf8SourceInfo->GetScriptContext());
    }

    return sourceObject;
}

Js::JavascriptArray * JsrtDebug::GetStackFrames(Js::ScriptContext* scriptContext, uint fromIndex, uint totalFrames)
{
    Js::JavascriptArray* stackTraceArray = scriptContext->GetLibrary()->CreateArray();

    // Review (SaAgarwa): Do I need to enumerate all script contexts and get frame pointers, based on stack address of the frame stitch together
    // Take care of architecture differences between x86, amd64 and ARM

    uint frameCount = 0;

    // Review (SaAgarwa): Ignored all script contexts which are not in debug mode?
    for (Js::ScriptContext *tempScriptContext = this->threadContext->GetScriptContextList();
    tempScriptContext != nullptr && tempScriptContext->IsInDebugMode();
        tempScriptContext = tempScriptContext->next)
    {
        Js::WeakDiagStack * framePointers = tempScriptContext->GetDebugContext()->GetProbeContainer()->GetFramePointers();
        if (framePointers != nullptr)
        {
            Js::DiagStack* stackFrames = framePointers->GetStrongReference();
            if (stackFrames != nullptr)
            {
                int count = stackFrames->Count();
                for (int frameIndex = 0; frameIndex < count; ++frameIndex)
                {
                    Js::DiagStackFrame* stackFrame = stackFrames->Peek(frameIndex);

                    // ToDo (SaAgarwa): Remove this check when we have a way to filter out the debug sim frames
                    if (stackFrame->GetFunction()->GetUtf8SourceInfo()->GetHostSourceContext() != -2)
                    {
                        Js::DynamicObject* stackTraceObject = this->GetStackFrame(stackFrame, frameCount);

                        Js::Var marshaledObj = Js::CrossSite::MarshalVar(scriptContext, stackTraceObject);
                        Js::JavascriptOperators::OP_SetElementI((Js::Var)stackTraceArray, Js::JavascriptNumber::ToVar(frameCount++, scriptContext), marshaledObj, scriptContext);
                    }
                }
            }
            framePointers->ReleaseStrongReference();
        }
    }

    return stackTraceArray;
}

Js::DynamicObject * JsrtDebug::GetStackFrame(Js::DiagStackFrame * stackFrame, uint frameIndex)
{
    DebuggerObjectBase* debuggerObject = DebuggerObjectStackFrame::Make(this->GetDebuggerObjectsManager(), stackFrame, frameIndex);
    return debuggerObject->GetJSONObject(stackFrame->GetScriptContext());
}

HRESULT JsrtDebug::SetBreakPoint(Js::Utf8SourceInfo* utf8SourceInfo, UINT lineNumber, UINT columnNumber, UINT *breakpointId)
{
    Js::DebugDocument* debugDocument = utf8SourceInfo->GetDebugDocument();
    if (SUCCEEDED(utf8SourceInfo->EnsureLineOffsetCacheNoThrow()))
    {
        charcount_t charPosition;
        charcount_t byteOffset;
        utf8SourceInfo->GetCharPositionForLineInfo((charcount_t)lineNumber, &charPosition, &byteOffset);
        long ibos = charPosition + columnNumber + 1;

        Js::StatementLocation statement;
        if (!debugDocument->GetStatementLocation(ibos, &statement))
        {
            return E_FAIL;
        }

        // Find if a breakpoint already exists, if so just return the id of it
        UINT bpId = debugDocument->FindBreakpointId(statement);
        if (bpId > 0)
        {
            *breakpointId = bpId;
            return S_OK;
        }

        if (SUCCEEDED(debugDocument->SetBreakPoint(statement, BREAKPOINT_ENABLED, breakpointId)))
        {
            this->GetDebugDocumentManager()->AddDocument(*breakpointId, debugDocument);
            return S_OK;
        }
    }

    return E_FAIL;
}

void JsrtDebug::GetBreakpoints(Js::JavascriptArray** bpsArray, Js::ScriptContext * scriptContext)
{
    Js::ScriptContext* arrayScriptContext = (*bpsArray)->GetScriptContext();
    Js::ProbeContainer* probeContainer = scriptContext->GetDebugContext()->GetProbeContainer();
    probeContainer->MapProbes([&](int i, Js::Probe* pProbe) {

        Js::BreakpointProbe* bp = (Js::BreakpointProbe*)pProbe;
        Js::DynamicObject* bpObject = scriptContext->GetLibrary()->CreateObject();

        Js::Utf8SourceInfo* utf8SourceInfo = bp->GetDbugDocument()->GetUtf8SourceInfo();

        JsrtDebugUtils::AddDoublePropertyToObject(bpObject, L"id", bp->GetId(), scriptContext);
        JsrtDebugUtils::AddDoublePropertyToObject(bpObject, L"scriptId", utf8SourceInfo->GetSourceInfoId(), scriptContext);
        JsrtDebugUtils::AddSouceUrlToObject(bpObject, utf8SourceInfo);

        charcount_t lineNumber = 0;
        charcount_t column = 0;
        charcount_t byteOffset = 0;
        utf8SourceInfo->GetLineInfoForCharPosition(bp->GetCharacterOffset(), &lineNumber, &column, &byteOffset);

        JsrtDebugUtils::AddDoublePropertyToObject(bpObject, L"line", lineNumber, scriptContext);
        JsrtDebugUtils::AddDoublePropertyToObject(bpObject, L"column", column, scriptContext);

        Js::Var marshaledObj = Js::CrossSite::MarshalVar(arrayScriptContext, bpObject);
        Js::JavascriptOperators::OP_SetElementI((Js::Var)(*bpsArray), Js::JavascriptNumber::ToVar((*bpsArray)->GetLength(), arrayScriptContext), marshaledObj, arrayScriptContext);
    });
}

DebuggerObjectsManager * JsrtDebug::GetDebuggerObjectsManager()
{
    if (this->debuggerObjectsManager == nullptr)
    {
        this->debuggerObjectsManager = Anew(this->GetDebugObjectArena(), DebuggerObjectsManager, this);
    }
    return this->debuggerObjectsManager;
}

ArenaAllocator * JsrtDebug::GetDebugObjectArena()
{
    if (this->debugObjectArena == nullptr)
    {
        this->debugObjectArena = HeapNew(ArenaAllocator, L"DebugObjectArena", this->threadContext->GetDebugManager()->GetDiagnosticPageAllocator(), Js::Throw::OutOfMemory);
        this->threadContext->GetRecycler()->RegisterExternalGuestArena(this->debugObjectArena);
    }

    return this->debugObjectArena;
}

DebugDocumentManager * JsrtDebug::GetDebugDocumentManager()
{
    if (this->debugDocumentManager == nullptr)
    {
        this->debugDocumentManager = Anew(this->GetDebugObjectArena(), DebugDocumentManager, this);
    }
    return this->debugDocumentManager;
}

void JsrtDebug::ClearDebugDocument(Js::ScriptContext * scriptContext)
{
    if (this->debugDocumentManager != nullptr)
    {
        this->debugDocumentManager->ClearDebugDocument(scriptContext);
    }
}

void JsrtDebug::RemoveBreakpoint(UINT breakpointId)
{
    if (this->debugDocumentManager != nullptr)
    {
        this->GetDebugDocumentManager()->RemoveBreakpoint(breakpointId);
    }
}

void JsrtDebug::SetBreakOnException(JsDiagBreakOnExceptionType breakOnExceptionType)
{
    this->breakOnExceptionType = breakOnExceptionType;
}

JsDiagBreakOnExceptionType JsrtDebug::GetBreakOnException()
{
    return this->breakOnExceptionType;
}
