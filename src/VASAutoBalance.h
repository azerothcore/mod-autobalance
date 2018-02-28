#ifndef VAS_AUTOBALANCE_H
#define VAS_AUTOBALANCE_H

#include "ScriptMgr.h"
#include "Creature.h"

// Manages registration, loading, and execution of scripts.
class VasScriptMgr
{
    friend class ACE_Singleton<VasScriptMgr, ACE_Null_Mutex>;
    public: /* Initialization */

        bool OnBeforeModifyAttributes(Creature* creature);
};

#define sVasScriptMgr ACE_Singleton<VasScriptMgr, ACE_Null_Mutex>::instance()

/*
* Dedicated hooks for VasAutobalance Module
* Can be used to extend/customize this system
*/
class VasModuleScript : public ModuleScript
{
    protected:

        VasModuleScript(const char* name);

    public: 
        virtual bool OnBeforeModifyAttributes(Creature* /*creature*/) { return true; }
};

template class ScriptRegistry<VasModuleScript>;

#endif