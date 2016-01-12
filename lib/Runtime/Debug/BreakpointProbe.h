//-------------------------------------------------------------------------------------------------------
// Copyright (C) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
//-------------------------------------------------------------------------------------------------------
#pragma once

namespace Js
{
    class BreakpointProbe : public Probe
    {
        int characterOffset;
        int byteOffset;
        DebugDocument* debugDocument;
        FunctionBody* functionBody;
        UINT breakPointId;

    public:
        BreakpointProbe(DebugDocument* debugDocument, StatementLocation& statement);

        virtual bool Install(ScriptContext* pScriptContext);
        virtual bool Uninstall(ScriptContext* pScriptContext);
        virtual bool CanHalt(InterpreterHaltState* pHaltState);
        virtual void DispatchHalt(InterpreterHaltState* pHaltState);
        virtual void CleanupHalt();

        bool Matches(FunctionBody* _pBody, int characterPosition);
        bool Matches(StatementLocation statement);

        UINT GetId() const { return this->breakPointId; }
        void GetStatementLocation(StatementLocation * statement);

        DebugDocument* GetDbugDocument() { return this->debugDocument; }
        int GetCharacterOffset() { return this->characterOffset; }
    };

    typedef JsUtil::List<BreakpointProbe*, ArenaAllocator> BreakpointProbeList;
}
