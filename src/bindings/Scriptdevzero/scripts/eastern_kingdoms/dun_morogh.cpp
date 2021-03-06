/* Copyright (C) 2006 - 2011 ScriptDev2 <https://scriptdev2.svn.sourceforge.net/>
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* ScriptData
SDName: Dun_Morogh
SD%Complete: 50
SDComment: Quest support: 1783
SDCategory: Dun Morogh
EndScriptData */

/* ContentData
npc_narm_faulk
EndContentData */

#include "precompiled.h"
#include "sc_gossip.h"

/*######
## npc_narm_faulk
######*/

#define SAY_HEAL    -1000187

struct MANGOS_DLL_DECL npc_narm_faulkAI : public ScriptedAI
{
    uint32 lifeTimer;
    bool spellHit;

    npc_narm_faulkAI(Creature* pCreature) : ScriptedAI(pCreature) {Reset();}

    void Reset()
    {
        lifeTimer = 120000;
        m_creature->SetUInt32Value(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_DEAD);
        m_creature->SetStandState(UNIT_STAND_STATE_DEAD);
        spellHit = false;
    }

    void MoveInLineOfSight(Unit* /*who*/) { }

    void UpdateAI(const uint32 diff)
    {
        if (m_creature->IsStandState())
        {
            if (lifeTimer < diff)
                m_creature->AI()->ResetToHome();
            else
                lifeTimer -= diff;
        }
    }

    void SpellHit(Unit *Hitter, const SpellEntry *Spellkind)
    {
        if (Spellkind->Id == 8593 && !spellHit)
        {
            DoCastSpellIfCan(m_creature,32343);
            m_creature->SetStandState(UNIT_STAND_STATE_STAND);
            m_creature->SetUInt32Value(UNIT_DYNAMIC_FLAGS, 0);
            //m_creature->RemoveAllAuras();
            DoScriptText(SAY_HEAL, m_creature, Hitter);
            spellHit = true;
        }
    }

};

CreatureAI* GetAI_npc_narm_faulk(Creature* pCreature)
{
    return new npc_narm_faulkAI(pCreature);
}

/********
** Jarven Thuunderbrew and Barrels
****/

enum JarvenThunderbrew
{
    QUEST_DISTRACTING_JARVEN = 308,
    GUARDED_BARREL = 269,
    UNGUARDED_BARREL = 270,
    GUARDED_BARREL_GOSSIP = 9027,
};

static const float m_guardedBarrelCoords[3] = { -5607.48f, -547.992f, 392.986f };

struct MANGOS_DLL_DECL npc_jarven_thunderbrew : public ScriptedAI
{
    npc_jarven_thunderbrew(Creature* pCreature) : ScriptedAI(pCreature) {Reset();}

    uint32 m_relocateTimer;
    GameObject *guardedBarrel;

    void Reset()
    {
        m_relocateTimer = 2000;
    }

    void StartRelocateTimer()
    {
        m_relocateTimer = 30000;
    }

    void UpdateAI(const uint32 uiDiff)
    {
        if(m_relocateTimer)
        {
            if(m_relocateTimer <= uiDiff)
            {
                if(!GetClosestGameObjectWithEntry(m_creature, GUARDED_BARREL, 10.0f))
                {
                    m_creature->SummonGameObject(GUARDED_BARREL, 0, m_guardedBarrelCoords[0], m_guardedBarrelCoords[1], m_guardedBarrelCoords[2], 0.0f);
                }
                m_relocateTimer = 0;
            }
            else
                m_relocateTimer -= uiDiff;
        }
    }

};

CreatureAI* GetAI_npc_jarven_thunderbrew(Creature* pCreature)
{
    return new npc_jarven_thunderbrew(pCreature);
}

bool pQuestRewardedNPC_npc_jarven_thunderbrew(Player* /*pPlayer*/, Creature* pCreature, const Quest* pQuest)
{
	if(pCreature)
    {
        if (pQuest->GetQuestId() == QUEST_DISTRACTING_JARVEN)
        {
            if(GetClosestGameObjectWithEntry(pCreature, GUARDED_BARREL, 10.0f))
            {
                if (npc_jarven_thunderbrew* jarvenAI = dynamic_cast<npc_jarven_thunderbrew*>(pCreature->AI()))
                {
                    pCreature->RemoveFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_QUESTGIVER);
                    if(GameObject *guardedBarrel = GetClosestGameObjectWithEntry(pCreature, GUARDED_BARREL, 10.0f))
                    {
                        guardedBarrel->Delete();
                    }
                    pCreature->SummonGameObject(UNGUARDED_BARREL, 30000, m_guardedBarrelCoords[0], m_guardedBarrelCoords[1], m_guardedBarrelCoords[2], 0.0f);
                    jarvenAI->StartRelocateTimer();
                }
            }
		
        }
    }
    return false;
}

bool GOuse_guarded_barrel (Player* pPlayer, GameObject* pGameobject)
{
    if(pPlayer && pGameobject)
    {
        pPlayer->SEND_GOSSIP_MENU(GUARDED_BARREL_GOSSIP, pGameobject->GetGUID());
    }

    return true;
}

/*######
## npc_avarus_kharag
######*/

enum eAvaraus
{
    SPELL_HOLY_LIGHT = 1026,
    SPELL_HAMMER_OF_JUSTICE  = 5588
};

struct MANGOS_DLL_DECL npc_avarus_kharagAI : public ScriptedAI
{
    uint32 m_uiHealTimer;
    uint32 m_uiSelfHealTimer;
    uint32 m_uiHammerOfJusticeTimer;
    bool m_bCanHeal;

    npc_avarus_kharagAI(Creature* pCreature) : ScriptedAI(pCreature) {Reset();}

    void Reset()
    {
        m_uiHealTimer = 12000;
        m_uiSelfHealTimer = urand(20000, 30000);
        m_uiHammerOfJusticeTimer = urand(6000, 8000);
        m_bCanHeal = true;
    }

    void MoveInLineOfSight(Unit* who)
    {
        if(m_bCanHeal && (who->IsFriendlyTo(m_creature) && who->GetHealthPercent() < 80 && who->GetTypeId() == TYPEID_PLAYER))
        {
            DoCastSpellIfCan(who, SPELL_HOLY_LIGHT);
            m_bCanHeal = false;
            m_uiHealTimer = 10000;
        }
        ScriptedAI::MoveInLineOfSight(who);
    }

    void UpdateAI(const uint32 diff)
    {
        if (m_uiHealTimer < diff)
            m_bCanHeal = true;
        else
            m_uiHealTimer -= diff;
            
        if (!m_creature->SelectHostileTarget() || !m_creature->getVictim())
            return;
        
        if (m_uiSelfHealTimer < diff && m_creature->GetHealthPercent() < 31)
        {
            DoCastSpellIfCan(m_creature, SPELL_HOLY_LIGHT);
            m_uiSelfHealTimer = urand(20000, 30000); 
        }
        else
            m_uiSelfHealTimer -= diff;
        
        if (m_uiHammerOfJusticeTimer < diff)
        {
            DoCastSpellIfCan(m_creature->getVictim(), SPELL_HAMMER_OF_JUSTICE);
            m_uiHammerOfJusticeTimer = urand(14000, 20000);
        }
        else
            m_uiHammerOfJusticeTimer -= diff;
        
        DoMeleeAttackIfReady();
    }
};

CreatureAI* GetAI_npc_avarus_kharag(Creature* pCreature)
{
    return new npc_avarus_kharagAI(pCreature);
}

void AddSC_dun_morogh()
{
    Script* pNewscript;

    pNewscript = new Script;
    pNewscript->Name = "npc_narm_faulk";
    pNewscript->GetAI = &GetAI_npc_narm_faulk;
    pNewscript->RegisterSelf();

    pNewscript = new Script;
    pNewscript->Name = "npc_jarven_thunderbrew";
    pNewscript->pQuestRewardedNPC = &pQuestRewardedNPC_npc_jarven_thunderbrew;
    pNewscript->GetAI = &GetAI_npc_jarven_thunderbrew;
    pNewscript->RegisterSelf();

    pNewscript = new Script;
    pNewscript->pGOUse = &GOuse_guarded_barrel;
    pNewscript->Name = "go_use_guarded_barrel";
    pNewscript->RegisterSelf();

    pNewscript = new Script;
    pNewscript->Name = "npc_avarus_kharag";
    pNewscript->GetAI = &GetAI_npc_avarus_kharag;
    pNewscript->RegisterSelf();
}
