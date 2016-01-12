//---------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
//----------------------------------------------------------------------------

#include "JsrtPch.h"
#include "JsrtDebugEventObject.h"
#include "..\Runtime\Debug\RuntimeDebugPch.h"
#include "screrror.h"   // For CompileScriptException

JsrtDebugEventObject::JsrtDebugEventObject(Js::ScriptContext *scriptContext)
{
    Assert(scriptContext != nullptr);
    this->scriptContext = scriptContext;
    this->eventDataObject = scriptContext->GetLibrary()->CreateObject();
    Assert(this->eventDataObject != nullptr);
}

JsrtDebugEventObject::~JsrtDebugEventObject()
{
    this->eventDataObject = nullptr;
    this->scriptContext = nullptr;
}

Js::DynamicObject* JsrtDebugEventObject::GetEventDataObject()
{
    return this->eventDataObject;
}

DebugDocumentManager::DebugDocumentManager(JsrtDebug* debugObject)
{
    Assert(debugObject != nullptr);
    this->debugObject = debugObject;
}

DebugDocumentManager::~DebugDocumentManager()
{
    if (this->breakpointDebugDocumentDictionary != nullptr)
    {
        AssertMsg(this->breakpointDebugDocumentDictionary->Count() == 0, "Should have cleared all entries by now?");

        HeapDelete(this->breakpointDebugDocumentDictionary);
        this->breakpointDebugDocumentDictionary = nullptr;
    }
    this->debugObject = nullptr;
}

void DebugDocumentManager::AddDocument(UINT bpId, Js::DebugDocument * debugDocument)
{
    BreakpointDebugDocumentDictionary* breakpointDebugDocumentDictionary = this->GetBreakpointDictionary();

    Assert(!breakpointDebugDocumentDictionary->ContainsKey(bpId));

    breakpointDebugDocumentDictionary->Add(bpId, debugDocument);
}

void DebugDocumentManager::ClearDebugDocument(Js::ScriptContext * scriptContext)
{
    if (scriptContext != nullptr && this->breakpointDebugDocumentDictionary != nullptr)
    {
        scriptContext->MapScript([&](Js::Utf8SourceInfo* sourceInfo)
        {
            if (sourceInfo->HasDebugDocument())
            {
                Js::DebugDocument* debugDocument = sourceInfo->GetDebugDocument();
                this->breakpointDebugDocumentDictionary->MapAndRemoveIf([&](JsUtil::SimpleDictionaryEntry<UINT, Js::DebugDocument *> keyValue)
                {
                    if (keyValue.Value() != nullptr && keyValue.Value() == debugDocument)
                    {
                        return true;
                    }
                    return false;
                });
            }
        });
    }
}

void DebugDocumentManager::RemoveBreakpoint(UINT breakpointId)
{
    if (this->breakpointDebugDocumentDictionary != nullptr)
    {
        BreakpointDebugDocumentDictionary* breakpointDebugDocumentDictionary = this->GetBreakpointDictionary();
        Js::DebugDocument* debugDocument = nullptr;
        if (breakpointDebugDocumentDictionary->TryGetValue(breakpointId, &debugDocument))
        {
            Js::StatementLocation statement;
            if (debugDocument->FindBPStatementLocation(breakpointId, &statement))
            {
                debugDocument->SetBreakPoint(statement, BREAKPOINT_DELETED);
            }
        }
    }
}

DebugDocumentManager::BreakpointDebugDocumentDictionary * DebugDocumentManager::GetBreakpointDictionary()
{
    if (this->breakpointDebugDocumentDictionary == nullptr)
    {
        this->breakpointDebugDocumentDictionary = Anew(this->debugObject->GetDebugObjectArena(), BreakpointDebugDocumentDictionary, this->debugObject->GetDebugObjectArena(), 10);
    }
    return breakpointDebugDocumentDictionary;
}
