/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2011 MaNGOSZero <https://github.com/mangos-zero>
 *
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

#include "Spell.h"
#include "Database/DatabaseEnv.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Opcodes.h"
#include "Log.h"
#include "UpdateMask.h"
#include "World.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "Player.h"
#include "Pet.h"
#include "Unit.h"
#include "DynamicObject.h"
#include "Group.h"
#include "UpdateData.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "CellImpl.h"
#include "Policies/SingletonImp.h"
#include "SharedDefines.h"
#include "LootMgr.h"
#include "VMapFactory.h"
#include "BattleGround.h"
#include "extras/Mod.h"
#include "Util.h"
#include "Chat.h"
#include "Object.h"
#include "Totem.h"
#include "MoveMap.h"
#include "MoveMapSharedDefines.h"
#include "PathFinder.h"

#define SPELL_CHANNEL_UPDATE_INTERVAL (1 * IN_MILLISECONDS)

extern pEffect SpellEffects[TOTAL_SPELL_EFFECTS];

bool IsQuestTameSpell(uint32 spellId)
{
    SpellEntry const *spellproto = sSpellStore.LookupEntry(spellId);
    if (!spellproto)
        return false;

    return spellproto->Effect[EFFECT_INDEX_0] == SPELL_EFFECT_THREAT
           && spellproto->Effect[EFFECT_INDEX_1] == SPELL_EFFECT_APPLY_AURA && spellproto->EffectApplyAuraName[EFFECT_INDEX_1] == SPELL_AURA_DUMMY;
}

SpellCastTargets::SpellCastTargets()
{
    m_unitTarget = NULL;
    m_itemTarget = NULL;
    m_GOTarget   = NULL;

    m_itemTargetEntry  = 0;

    m_srcX = m_srcY = m_srcZ = m_destX = m_destY = m_destZ = 0.0f;
    m_strTarget = "";
    m_targetMask = 0;
}

SpellCastTargets::~SpellCastTargets()
{
}

void SpellCastTargets::setUnitTarget(Unit *target)
{
    if (!target)
        return;

    m_destX = target->GetPositionX();
    m_destY = target->GetPositionY();
    m_destZ = target->GetPositionZ();
    m_unitTarget = target;
    m_unitTargetGUID = target->GetObjectGuid();
    m_targetMask |= TARGET_FLAG_UNIT;
}

void SpellCastTargets::setDestination(float x, float y, float z)
{
    m_destX = x;
    m_destY = y;
    m_destZ = z;
    m_targetMask |= TARGET_FLAG_DEST_LOCATION;
}

void SpellCastTargets::setSource(float x, float y, float z)
{
    m_srcX = x;
    m_srcY = y;
    m_srcZ = z;
    m_targetMask |= TARGET_FLAG_SOURCE_LOCATION;
}

void SpellCastTargets::setGOTarget(GameObject *target)
{
    m_GOTarget = target;
    m_GOTargetGUID = target->GetObjectGuid();
    //    m_targetMask |= TARGET_FLAG_OBJECT;
}

void SpellCastTargets::setItemTarget(Item* item)
{
    if(!item)
        return;

    m_itemTarget = item;
    m_itemTargetGUID = item->GetObjectGuid();
    m_itemTargetEntry = item->GetEntry();
    m_targetMask |= TARGET_FLAG_ITEM;
}

void SpellCastTargets::setTradeItemTarget(Player* caster)
{
    m_itemTargetGUID = ObjectGuid(uint64(TRADE_SLOT_NONTRADED));
    m_itemTargetEntry = 0;
    m_targetMask |= TARGET_FLAG_TRADE_ITEM;

    Update(caster);
}

void SpellCastTargets::setCorpseTarget(Corpse* corpse)
{
    m_CorpseTargetGUID = corpse->GetObjectGuid();
}

void SpellCastTargets::Update(Unit* caster)
{
    m_GOTarget   = m_GOTargetGUID ? caster->GetMap()->GetGameObject(m_GOTargetGUID) : NULL;
    m_unitTarget = m_unitTargetGUID ?
                   ( m_unitTargetGUID == caster->GetObjectGuid() ? caster : ObjectAccessor::GetUnit(*caster, m_unitTargetGUID) ) :
                       NULL;

    m_itemTarget = NULL;
    if (caster->GetTypeId() == TYPEID_PLAYER)
{
        Player *player = ((Player*)caster);

        if (m_targetMask & TARGET_FLAG_ITEM)
            m_itemTarget = player->GetItemByGuid(m_itemTargetGUID);
        else if (m_targetMask & TARGET_FLAG_TRADE_ITEM)
        {
            if (TradeData* pTrade = player->GetTradeData())
                if (m_itemTargetGUID.GetRawValue() < TRADE_SLOT_COUNT)
                    m_itemTarget = pTrade->GetTraderData()->GetItem(TradeSlots(m_itemTargetGUID.GetRawValue()));
        }

        if (m_itemTarget)
            m_itemTargetEntry = m_itemTarget->GetEntry();
    }
}

void SpellCastTargets::read( ByteBuffer& data, Unit *caster )
{
    data >> m_targetMask;

    if(m_targetMask == TARGET_FLAG_SELF)
    {
        m_destX = caster->GetPositionX();
        m_destY = caster->GetPositionY();
        m_destZ = caster->GetPositionZ();
        m_unitTarget = caster;
        m_unitTargetGUID = caster->GetObjectGuid();
        return;
    }

    // TARGET_FLAG_UNK2 is used for non-combat pets, maybe other?
    if( m_targetMask & ( TARGET_FLAG_UNIT | TARGET_FLAG_UNK2 ))
        data >> m_unitTargetGUID.ReadAsPacked();

    if( m_targetMask & ( TARGET_FLAG_OBJECT | TARGET_FLAG_OBJECT_UNK ))
        data >> m_GOTargetGUID.ReadAsPacked();

    if(( m_targetMask & ( TARGET_FLAG_ITEM | TARGET_FLAG_TRADE_ITEM )) && caster->GetTypeId() == TYPEID_PLAYER)
        data >> m_itemTargetGUID.ReadAsPacked();

    if (m_targetMask & TARGET_FLAG_SOURCE_LOCATION)
    {
        data >> m_srcX >> m_srcY >> m_srcZ;
        if(!MaNGOS::IsValidMapCoord(m_srcX, m_srcY, m_srcZ))
            throw ByteBufferException(false, data.rpos(), 0, data.size());
    }

    if (m_targetMask & TARGET_FLAG_DEST_LOCATION)
    {
        data >> m_destX >> m_destY >> m_destZ;
        if(!MaNGOS::IsValidMapCoord(m_destX, m_destY, m_destZ))
            throw ByteBufferException(false, data.rpos(), 0, data.size());
    }

    if( m_targetMask & TARGET_FLAG_STRING )
        data >> m_strTarget;

    if( m_targetMask & (TARGET_FLAG_CORPSE | TARGET_FLAG_PVP_CORPSE ) )
        data >> m_CorpseTargetGUID.ReadAsPacked();

    // find real units/GOs
    Update(caster);
}

void SpellCastTargets::write( ByteBuffer& data ) const
{
    data << uint16(m_targetMask);

    if( m_targetMask & ( TARGET_FLAG_UNIT | TARGET_FLAG_PVP_CORPSE | TARGET_FLAG_OBJECT | TARGET_FLAG_CORPSE | TARGET_FLAG_UNK2 ) )
    {
        if(m_targetMask & TARGET_FLAG_UNIT)
        {
            if(m_unitTarget)
                data << m_unitTarget->GetPackGUID();
            else
                data << uint8(0);
        }
        else if( m_targetMask & ( TARGET_FLAG_OBJECT | TARGET_FLAG_OBJECT_UNK ) )
        {
            if(m_GOTarget)
                data << m_GOTarget->GetPackGUID();
            else
                data << uint8(0);
        }
        else if( m_targetMask & ( TARGET_FLAG_CORPSE | TARGET_FLAG_PVP_CORPSE ) )
            data << m_CorpseTargetGUID.WriteAsPacked();
        else
            data << uint8(0);
    }

    if( m_targetMask & ( TARGET_FLAG_ITEM | TARGET_FLAG_TRADE_ITEM ) )
    {
        if(m_itemTarget)
            data << m_itemTarget->GetPackGUID();
        else
            data << uint8(0);
    }

    if (m_targetMask & TARGET_FLAG_SOURCE_LOCATION)
        data << m_srcX << m_srcY << m_srcZ;

    if (m_targetMask & TARGET_FLAG_DEST_LOCATION)
        data << m_destX << m_destY << m_destZ;

    if( m_targetMask & TARGET_FLAG_STRING )
        data << m_strTarget;
}

Spell::Spell( Unit* caster, SpellEntry const *info, bool triggered, ObjectGuid originalCasterGUID, SpellEntry const* triggeredBy )
{
    MANGOS_ASSERT( caster != NULL && info != NULL );
    MANGOS_ASSERT( info == sSpellStore.LookupEntry( info->Id ) && "`info` must be pointer to sSpellStore element");

    m_spellInfo = info;
    m_triggeredBySpellInfo = triggeredBy;
    m_caster = caster;
    m_selfContainer = NULL;
    m_referencedFromCurrentSpell = false;
    m_executedCurrently = false;
    m_delayStart = 0;
    m_delayAtDamageCount = 0;

    m_applyMultiplierMask = 0;

    // Get data for type of attack
    m_attackType = GetWeaponAttackType(m_spellInfo);

    m_spellSchoolMask = GetSpellSchoolMask(info);           // Can be override for some spell (wand shoot for example)

    if(m_attackType == RANGED_ATTACK)
    {
        // wand case
        if((m_caster->getClassMask() & CLASSMASK_WAND_USERS) != 0 && m_caster->GetTypeId() == TYPEID_PLAYER)
        {
            if(Item* pItem = ((Player*)m_caster)->GetWeaponForAttack(RANGED_ATTACK))
                m_spellSchoolMask = GetSchoolMask(pItem->GetProto()->Damage[0].DamageType);
        }
    }


    if(m_spellInfo->Id == 2687 || m_spellInfo->Id == 5229) // Bloodrage (warrior) & Enrage (druid).
    {
        caster->SetInCombatState(false, caster); // Put player in combat.
    }

    // Set health leech amount to zero
    m_healthLeech = 0;

    m_originalCasterGUID = originalCasterGUID ? originalCasterGUID : m_caster->GetObjectGuid();

    UpdateOriginalCasterPointer();

    for(int i = 0; i < MAX_EFFECT_INDEX; ++i)
        m_currentBasePoints[i] = m_spellInfo->CalculateSimpleValue(SpellEffectIndex(i));

    m_spellState = SPELL_STATE_NULL;

    m_castPositionX = m_castPositionY = m_castPositionZ = 0;
    m_TriggerSpells.clear();
    m_preCastSpells.clear();
    m_IsTriggeredSpell = triggered;
    //m_AreaAura = false;
    m_CastItem = NULL;

    unitTarget = NULL;
    itemTarget = NULL;
    gameObjTarget = NULL;
    focusObject = NULL;
    m_triggeredByAuraSpell  = NULL;

    //Auto Shot & Shoot
    m_autoRepeat = IsAutoRepeatRangedSpell(m_spellInfo);

    m_powerCost = 0;                                        // setup to correct value in Spell::prepare, don't must be used before.
    m_casttime = 0;                                         // setup to correct value in Spell::prepare, don't must be used before.
    m_startedCasting = 0;									// When we begin to cast the spell this will be initialized- used to determine whether to use proc buffs
    m_timer = 0;                                            // will set to cast time in prepare
    m_duration = 0;

    m_needAliveTargetMask = 0;

    // determine reflection
    m_canReflect = false;

    if(m_spellInfo->DmgClass == SPELL_DAMAGE_CLASS_MAGIC && !(m_spellInfo->AttributesEx2 & SPELL_ATTR_EX2_CANT_REFLECTED))
    {
        for(int j = 0; j < MAX_EFFECT_INDEX; ++j)
        {
            if (m_spellInfo->Effect[j] == 0)
                continue;

            if(!IsPositiveTarget(m_spellInfo->EffectImplicitTargetA[j], m_spellInfo->EffectImplicitTargetB[j]))
                m_canReflect = true;
            else
                m_canReflect = (m_spellInfo->AttributesEx & SPELL_ATTR_EX_NEGATIVE) ? true : false;

            if(m_canReflect)
                continue;
            else
                break;
        }
    }

    CleanupTargetList();
}

Spell::~Spell()
{
}

template<typename T>
WorldObject* Spell::FindCorpseUsing()
{
    // non-standard target selection
    SpellRangeEntry const* srange = sSpellRangeStore.LookupEntry(m_spellInfo->rangeIndex);
    float max_range = GetSpellMaxRange(srange);

    WorldObject* result = NULL;

    T u_check(m_caster, max_range);
    MaNGOS::WorldObjectSearcher<T> searcher(result, u_check);

    Cell::VisitGridObjects(m_caster, searcher, max_range);

    if (!result)
        Cell::VisitWorldObjects(m_caster, searcher, max_range);

    return result;
}

void Spell::FillTargetMap()
{
    // TODO: ADD the correct target FILLS!!!!!!

    for(int i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        // not call for empty effect.
        // Also some spells use not used effect targets for store targets for dummy effect in triggered spells
        if(m_spellInfo->Effect[i] == 0)
            continue;

        // targets for TARGET_SCRIPT_COORDINATES (A) and TARGET_SCRIPT
        // for TARGET_FOCUS_OR_SCRIPTED_GAMEOBJECT (A) all is checked in Spell::CheckCast and in Spell::CheckItem
        // filled in Spell::CheckCast call
        if(m_spellInfo->EffectImplicitTargetA[i] == TARGET_SCRIPT_COORDINATES ||
                m_spellInfo->EffectImplicitTargetA[i] == TARGET_SCRIPT ||
                m_spellInfo->EffectImplicitTargetA[i] == TARGET_FOCUS_OR_SCRIPTED_GAMEOBJECT ||
                (m_spellInfo->EffectImplicitTargetB[i] == TARGET_SCRIPT && m_spellInfo->EffectImplicitTargetA[i] != TARGET_SELF))
            continue;

        // TODO: find a way so this is not needed?
        // for area auras always add caster as target (needed for totems for example)
        if(IsAreaAuraEffect(m_spellInfo->Effect[i]))
            AddUnitTarget(m_caster, SpellEffectIndex(i));

        UnitList tmpUnitMap;

        // TargetA/TargetB dependent from each other, we not switch to full support this dependences
        // but need it support in some know cases
        switch(m_spellInfo->EffectImplicitTargetA[i])
        {
        case 0:
            switch(m_spellInfo->EffectImplicitTargetB[i])
            {
            case 0:
                SetTargetMap(SpellEffectIndex(i), TARGET_EFFECT_SELECT, tmpUnitMap);
                break;
            default:
                SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitMap);
                break;
            }
            break;
        case TARGET_SELF:
            switch(m_spellInfo->EffectImplicitTargetB[i])
            {
            case 0:
                // Arcane Missiles have strange targeting for auras
                if(m_spellInfo->SpellFamilyName == SPELLFAMILY_MAGE && m_spellInfo->SpellFamilyFlags & UI64LIT(0x00000800))
                {
                    if (m_caster->GetTypeId() == TYPEID_PLAYER)
                        if (Unit *target = ObjectAccessor::Instance().GetUnit(*m_caster, ((Player*)m_caster)->GetSelectionGuid()))
                            if (!m_caster->IsFriendlyTo(target))
                                tmpUnitMap.push_back(target);
                }
                else
                    SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitMap);
                break;
            case TARGET_EFFECT_SELECT:
                SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitMap);
                break;
            case TARGET_AREAEFFECT_INSTANT:         // use B case that not dependent from from A in fact
                if((m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION)==0)
                    m_targets.setDestination(m_caster->GetPositionX(),m_caster->GetPositionY(),m_caster->GetPositionZ());
                SetTargetMap(SpellEffectIndex(i),m_spellInfo->EffectImplicitTargetB[i],tmpUnitMap);
                break;
            default:
                SetTargetMap(SpellEffectIndex(i),m_spellInfo->EffectImplicitTargetA[i],tmpUnitMap);
                SetTargetMap(SpellEffectIndex(i),m_spellInfo->EffectImplicitTargetB[i],tmpUnitMap);
                break;
            }
            break;
        case TARGET_EFFECT_SELECT:
            switch(m_spellInfo->EffectImplicitTargetB[i])
            {
            case 0:
            case TARGET_EFFECT_SELECT:
                SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitMap);
                break;
                // dest point setup required
            case TARGET_AREAEFFECT_INSTANT:
            case TARGET_AREAEFFECT_CUSTOM:
            case TARGET_ALL_ENEMY_IN_AREA:
            case TARGET_ALL_ENEMY_IN_AREA_INSTANT:
            case TARGET_ALL_ENEMY_IN_AREA_CHANNELED:
            case TARGET_ALL_FRIENDLY_UNITS_IN_AREA:
                // triggered spells get dest point from default target set, ignore it
                switch (m_spellInfo->Id)
                {
                case 17731:
                    /* Don't use caster position for centerpoint in Eruption */
                    break;
                default:
                    if (!(m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION) || m_IsTriggeredSpell)
                        if (WorldObject* castObject = GetCastingObject())
                            m_targets.setDestination(castObject->GetPositionX(), castObject->GetPositionY(), castObject->GetPositionZ());
                }
                SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitMap);
                break;
                // target pre-selection required
            case TARGET_INNKEEPER_COORDINATES:
            case TARGET_TABLE_X_Y_Z_COORDINATES:
            case TARGET_CASTER_COORDINATES:
            case TARGET_SCRIPT_COORDINATES:
            case TARGET_CURRENT_ENEMY_COORDINATES:
            case TARGET_DUELVSPLAYER_COORDINATES:
                // need some target for processing
                SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitMap);
                SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitMap);
                break;
            default:
                SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitMap);
                break;
            }
            break;
        case TARGET_CASTER_COORDINATES:
            switch(m_spellInfo->EffectImplicitTargetB[i])
            {
            case TARGET_ALL_ENEMY_IN_AREA:
                // Note: this hack with search required until GO casting not implemented
                // environment damage spells already have around enemies targeting but this not help in case nonexistent GO casting support
                // currently each enemy selected explicitly and self cast damage
                if (m_spellInfo->Effect[i] == SPELL_EFFECT_ENVIRONMENTAL_DAMAGE)
                {
                    if(m_targets.getUnitTarget())
                        tmpUnitMap.push_back(m_targets.getUnitTarget());
                }
                else
                {
                    SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitMap);
                    SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitMap);
                }
                break;
            case 0:
                SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitMap);
                tmpUnitMap.push_back(m_caster);
                break;
            case TARGET_ALL_FRIENDLY_UNITS_AROUND_CASTER:
            {
                SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitMap);
                SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitMap);

                // For the mage debuff at Nefarian in BWL.
                if (m_spellInfo->Id == 23603 && !tmpUnitMap.empty())
                {
                    uint32 target_count = urand(0, tmpUnitMap.size() - 1);
                    uint32 count = 0;
                    auto itr = tmpUnitMap.begin();

                    while (count < target_count)
                    {
                        ++itr;
                        count++;
                    }

                    Unit* new_target = *itr;

                    tmpUnitMap.clear();
                    tmpUnitMap.push_back(new_target);
                }

                break;
            }

            default:
            {
                // For Nefarian's Corruption at Vaelastrasz.
                if (m_spellInfo->Id == 23642 && m_targets.getUnitTarget() && m_targets.getUnitTarget()->GetEntry() == 13020)
                {
                    tmpUnitMap.push_back(m_targets.getUnitTarget());
                    break;
                }

                SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitMap);
                SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitMap);
                break;
            }
            }
            break;
        case TARGET_TABLE_X_Y_Z_COORDINATES:
            switch(m_spellInfo->EffectImplicitTargetB[i])
            {
            case 0:
                SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitMap);

                // need some target for processing
                SetTargetMap(SpellEffectIndex(i), TARGET_EFFECT_SELECT, tmpUnitMap);
                break;
            case TARGET_AREAEFFECT_INSTANT:         // All 17/7 pairs used for dest teleportation, A processed in effect code
                SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitMap);
                break;
            default:
                SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitMap);
                SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitMap);
                break;
            }
            break;
        case TARGET_DUELVSPLAYER_COORDINATES:
            switch(m_spellInfo->EffectImplicitTargetB[i])
            {
            case 0:
            case TARGET_EFFECT_SELECT:
                SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitMap);
                if (Unit* currentTarget = m_targets.getUnitTarget())
                    tmpUnitMap.push_back(currentTarget);
                break;
            default:
                SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitMap);
                SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitMap);
                break;
            }
            break;
        default:
            switch(m_spellInfo->EffectImplicitTargetB[i])
            {
            case 0:
            case TARGET_EFFECT_SELECT:
                SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitMap);
                break;
            case TARGET_SCRIPT_COORDINATES:         // B case filled in CheckCast but we need fill unit list base at A case
                SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitMap);
                break;
            default:
                SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetA[i], tmpUnitMap);
                SetTargetMap(SpellEffectIndex(i), m_spellInfo->EffectImplicitTargetB[i], tmpUnitMap);
                break;
            }
            break;
        }

        if (m_caster->GetTypeId() == TYPEID_PLAYER)
        {
            Player *me = (Player*)m_caster;
            for (UnitList::const_iterator itr = tmpUnitMap.begin(); itr != tmpUnitMap.end(); ++itr)
            {
                Player *targetOwner = (*itr)->GetCharmerOrOwnerPlayerOrPlayerItself();
                if (targetOwner && targetOwner != me && targetOwner->IsPvP() && !me->IsInDuelWith(targetOwner))
                {
                    me->UpdatePvP(true);
                    me->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_ENTER_PVP_COMBAT);
                    break;
                }
            }
        }

        for (UnitList::iterator itr = tmpUnitMap.begin(); itr != tmpUnitMap.end();)
        {
            if (!CheckTarget (*itr, SpellEffectIndex(i)))
            {
                itr = tmpUnitMap.erase(itr);
                continue;
            }
            else
                ++itr;
        }

        for(UnitList::const_iterator iunit = tmpUnitMap.begin(); iunit != tmpUnitMap.end(); ++iunit)
            AddUnitTarget((*iunit), SpellEffectIndex(i));
    }
}

void Spell::prepareDataForTriggerSystem()
{
    //==========================================================================================
    // Now fill data for trigger system, need know:
    // an spell trigger another or not ( m_canTrigger )
    // Create base triggers flags for Attacker and Victim ( m_procAttacker and  m_procVictim)
    //==========================================================================================
    // Fill flag can spell trigger or not
    // TODO: possible exist spell attribute for this
    m_canTrigger = false;

    if (m_CastItem)
        m_canTrigger = false;         // Do not trigger from item cast spell
    else if (!m_IsTriggeredSpell)
        m_canTrigger = true;          // Normal cast - can trigger
    else if (!m_triggeredByAuraSpell)
        m_canTrigger = true;          // Triggered from SPELL_EFFECT_TRIGGER_SPELL - can trigger

    if (!m_canTrigger)                // Exceptions (some periodic triggers)
    {
        switch (m_spellInfo->SpellFamilyName)
        {
        case SPELLFAMILY_MAGE:
            // Arcane Missiles / Blizzard triggers need do it
            if (m_spellInfo->IsFitToFamilyMask(UI64LIT(0x0000000000200080)))
                m_canTrigger = true;
            break;
        case SPELLFAMILY_WARLOCK:
            // For Hellfire Effect / Rain of Fire / Seed of Corruption triggers need do it
            if (m_spellInfo->IsFitToFamilyMask(UI64LIT(0x0000800000000060)))
                m_canTrigger = true;
            break;
        case SPELLFAMILY_HUNTER:
            // Hunter Explosive Trap Effect/Immolation Trap Effect/Frost Trap Aura/Snake Trap Effect
            if (m_spellInfo->IsFitToFamilyMask(UI64LIT(0x0000200000000014)))
                m_canTrigger = true;
            break;
        case SPELLFAMILY_PALADIN:
            // For Holy Shock triggers need do it
            if (m_spellInfo->IsFitToFamilyMask(UI64LIT(0x0001000000200000)))
                m_canTrigger = true;
            break;
        default:
            break;
        }
    }

    // Get data for type of attack and fill base info for trigger
    switch (m_spellInfo->DmgClass)
    {
    case SPELL_DAMAGE_CLASS_MELEE:
        m_procAttacker = PROC_FLAG_SUCCESSFUL_MELEE_SPELL_HIT;
        if (m_attackType == OFF_ATTACK)
            m_procAttacker |= PROC_FLAG_SUCCESSFUL_OFFHAND_HIT;
        m_procVictim   = PROC_FLAG_TAKEN_MELEE_SPELL_HIT;
        break;
    case SPELL_DAMAGE_CLASS_RANGED:
        // Auto attack
        if (m_spellInfo->AttributesEx2 & SPELL_ATTR_EX2_AUTOREPEAT_FLAG)
        {
            m_procAttacker = PROC_FLAG_SUCCESSFUL_RANGED_HIT;
            m_procVictim   = PROC_FLAG_TAKEN_RANGED_HIT;
        }
        else // Ranged spell attack
        {
            m_procAttacker = PROC_FLAG_SUCCESSFUL_RANGED_SPELL_HIT;
            m_procVictim   = PROC_FLAG_TAKEN_RANGED_SPELL_HIT;
        }
        break;
    default:
        if (IsPositiveSpell(m_spellInfo->Id))                                 // Check for positive spell
        {
            m_procAttacker = PROC_FLAG_SUCCESSFUL_POSITIVE_SPELL;
            m_procVictim   = PROC_FLAG_TAKEN_POSITIVE_SPELL;
        }
        else if (m_spellInfo->AttributesEx2 & SPELL_ATTR_EX2_AUTOREPEAT_FLAG) // Wands auto attack
        {
            m_procAttacker = PROC_FLAG_SUCCESSFUL_RANGED_HIT;
            m_procVictim   = PROC_FLAG_TAKEN_RANGED_HIT;
        }
        else                                           // Negative spell
        {
            m_procAttacker = PROC_FLAG_SUCCESSFUL_NEGATIVE_SPELL_HIT;
            m_procVictim   = PROC_FLAG_TAKEN_NEGATIVE_SPELL_HIT;
        }
        break;
    }

    // some negative spells have positive effects to another or same targets
    // avoid triggering negative hit for only positive targets
    m_negativeEffectMask = 0x0;
    for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
        if (!IsPositiveEffect(m_spellInfo, SpellEffectIndex(i)))
            m_negativeEffectMask |= (1<<i);

    // Hunter traps spells (for Entrapment trigger)
    // Gives your Immolation Trap, Frost Trap, Explosive Trap, and Snake Trap ....
    if (m_spellInfo->SpellFamilyName == SPELLFAMILY_HUNTER && m_spellInfo->SpellFamilyFlags & UI64LIT(0x000020000000001C))
        m_procAttacker |= PROC_FLAG_ON_TRAP_ACTIVATION;

    //Make sure to include auras this might have proc'd from - frost trap triggers entrapment by pulsing a generic dummy spell
    if (m_triggeredByAuraSpell && m_triggeredByAuraSpell->SpellFamilyName == SPELLFAMILY_HUNTER && m_triggeredByAuraSpell->SpellFamilyFlags & UI64LIT(0x000020000000001C))
    {
        //Proc entrapment & whatnot
        m_canTrigger = true;
        m_procAttacker = PROC_FLAG_SUCCESSFUL_NEGATIVE_SPELL_HIT|PROC_FLAG_ON_TRAP_ACTIVATION;
        m_procVictim   = PROC_FLAG_TAKEN_NEGATIVE_SPELL_HIT;

        //Also whatever force it to be negative- dummy abilities are positive by default & there's no good pipeline to check the triggering aura
        //for a spell... should probably fix that.
        m_negativeEffectMask = 7;
    }
    
    // Seal of Command should be able to trigger things.
    if (m_spellInfo->Id == 20424) 
        m_canTrigger = true;
}

void Spell::CleanupTargetList()
{
    m_UniqueTargetInfo.clear();
    m_UniqueGOTargetInfo.clear();
    m_UniqueItemInfo.clear();
    m_delayMoment = 0;
}

void Spell::AddUnitTarget(Unit* pVictim, SpellEffectIndex effIndex)
{
    if (m_spellInfo->Effect[effIndex] == 0)
        return;

    // Check for effect immune skip if immuned
    bool immuned = pVictim->IsImmuneToSpellEffect(m_spellInfo, effIndex);

    ObjectGuid targetGUID = pVictim->GetObjectGuid();

    // Lookup target in already in list
    for(TargetList::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        if (targetGUID == ihit->targetGUID)                 // Found in list
        {
            if (!immuned)
                ihit->effectMask |= 1 << effIndex;          // Add only effect mask if not immuned
            return;
        }
    }

    // This is new target calculate data for him

    // Get spell hit result on target
    TargetInfo target;
    target.targetGUID = targetGUID;                         // Store target GUID
    target.effectMask = immuned ? 0 : 1 << effIndex;        // Store index of effect if not immuned
    target.processed  = false;                              // Effects not apply on target

    // Calculate hit result
    PartialResistInfo partialResist;
    target.missCondition = m_caster->SpellHitResult(pVictim, m_spellInfo, partialResist, m_canReflect);
    target.partialResist = partialResist;

    // spell fly from visual cast object
    WorldObject* affectiveObject = GetAffectiveCasterObject();

    // Spell have speed - need calculate incoming time
    if (m_spellInfo->speed > 0.0f && affectiveObject && pVictim != affectiveObject)
    {
        // calculate spell incoming interval
        float dist = affectiveObject->GetDistance(pVictim->GetPositionX(), pVictim->GetPositionY(), pVictim->GetPositionZ());
        if (dist < 5.0f)
            dist = 5.0f;
        target.timeDelay = (uint64) floor(dist / m_spellInfo->speed * 1000.0f);

        // Calculate minimum incoming time
        if (m_delayMoment == 0 || m_delayMoment>target.timeDelay)
            m_delayMoment = target.timeDelay;
    }
    else
        target.timeDelay = UI64LIT(0);

    // If target reflect spell back to caster
    if (target.missCondition == SPELL_MISS_REFLECT)
    {
        // Calculate reflected spell result on caster
        PartialResistInfo partialResist;
        target.reflectResult =  m_caster->SpellHitResult(m_caster, m_spellInfo, partialResist, m_canReflect);
        target.reflectPartialResist = target.reflectPartialResist;

        if (target.reflectResult == SPELL_MISS_REFLECT)     // Impossible reflect again, so simply deflect spell
            target.reflectResult = SPELL_MISS_PARRY;

        // Increase time interval for reflected spells by 1.5
        target.timeDelay += target.timeDelay >> 1;
    }
    else
        target.reflectResult = SPELL_MISS_NONE;

    // Add target to list
    m_UniqueTargetInfo.push_back(target);
}

void Spell::AddUnitTarget(ObjectGuid unitGuid, SpellEffectIndex effIndex)
{
    if (Unit* unit = m_caster->GetObjectGuid() == unitGuid ? m_caster : ObjectAccessor::GetUnit(*m_caster, unitGuid))
        AddUnitTarget(unit, effIndex);
}

void Spell::AddGOTarget(GameObject* pVictim, SpellEffectIndex effIndex)
{
    if (m_spellInfo->Effect[effIndex] == 0)
        return;

    ObjectGuid targetGUID = pVictim->GetObjectGuid();

    // Lookup target in already in list
    for(GOTargetList::iterator ihit = m_UniqueGOTargetInfo.begin(); ihit != m_UniqueGOTargetInfo.end(); ++ihit)
    {
        if (targetGUID == ihit->targetGUID)                 // Found in list
        {
            ihit->effectMask |= (1 << effIndex);            // Add only effect mask
            return;
        }
    }

    // This is new target calculate data for him

    GOTargetInfo target;
    target.targetGUID = targetGUID;
    target.effectMask = (1 << effIndex);
    target.processed  = false;                              // Effects not apply on target

    // spell fly from visual cast object
    WorldObject* affectiveObject = GetAffectiveCasterObject();

    // Spell have speed - need calculate incoming time
    if (m_spellInfo->speed > 0.0f && affectiveObject && pVictim != affectiveObject)
    {
        // calculate spell incoming interval
        float dist = affectiveObject->GetDistance(pVictim->GetPositionX(), pVictim->GetPositionY(), pVictim->GetPositionZ());
        if (dist < 5.0f)
            dist = 5.0f;
        target.timeDelay = (uint64) floor(dist / m_spellInfo->speed * 1000.0f);
        if (m_delayMoment == 0 || m_delayMoment > target.timeDelay)
            m_delayMoment = target.timeDelay;
    }
    else
        target.timeDelay = UI64LIT(0);

    // Add target to list
    m_UniqueGOTargetInfo.push_back(target);
}

void Spell::AddGOTarget(ObjectGuid goGuid, SpellEffectIndex effIndex)
{
    if (GameObject* go = m_caster->GetMap()->GetGameObject(goGuid))
        AddGOTarget(go, effIndex);
}

void Spell::AddItemTarget(Item* pitem, SpellEffectIndex effIndex)
{
    if (m_spellInfo->Effect[effIndex] == 0)
        return;

    // Lookup target in already in list
    for(ItemTargetList::iterator ihit = m_UniqueItemInfo.begin(); ihit != m_UniqueItemInfo.end(); ++ihit)
    {
        if (pitem == ihit->item)                            // Found in list
        {
            ihit->effectMask |= (1 << effIndex);            // Add only effect mask
            return;
        }
    }

    // This is new target add data

    ItemTargetInfo target;
    target.item       = pitem;
    target.effectMask = (1 << effIndex);
    m_UniqueItemInfo.push_back(target);
}

void Spell::DoAllEffectOnTarget(TargetInfo *target)
{
    if (target->processed)                                  // Check target
        return;
    target->processed = true;                               // Target checked in apply effects procedure

    // Get mask of effects for target
    uint32 mask = target->effectMask;

    // Fill base trigger info
    uint32 procAttacker = m_procAttacker;
    uint32 procVictim   = m_procVictim;
    uint32 procEx       = PROC_EX_NONE;

    //Don't process area auras since those are handled separately- we're just here for the procs!
    bool excludedPersistantAreaAura = false;
    for (uint32 i = 0; i < MAX_EFFECT_INDEX; i++)
    {
        if (this->m_spellInfo->Effect[i] == SPELL_EFFECT_PERSISTENT_AREA_AURA)
        {
            excludedPersistantAreaAura=true;
            mask &= ~(1 << i);
            if (m_negativeEffectMask & (1 << i))
            {
                procAttacker |= PROC_FLAG_SUCCESSFUL_AOE_SPELL_HIT;
                procVictim |= PROC_FLAG_TAKEN_AOE_SPELL_HIT;
            } else
            {
                procAttacker |= PROC_FLAG_SUCCESSFUL_POSITIVE_AOE_HIT;
                procVictim |= PROC_FLAG_TAKEN_POSITIVE_AOE;
            }
        }
    }

    Unit* unit = m_caster->GetObjectGuid() == target->targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, target->targetGUID);
    if (!unit)
        return;

    // Get original caster (if exist) and calculate damage/healing from him data
    Unit *real_caster = GetAffectiveCaster();
    // FIXME: in case wild GO heal/damage spells will be used target bonuses
    Unit *caster = real_caster ? real_caster : m_caster;

    SpellMissInfo missInfo = target->missCondition;
    // Need init unitTarget by default unit (can changed in code on reflect)
    // Or on missInfo!=SPELL_MISS_NONE unitTarget undefined (but need in trigger subsystem)
    unitTarget = unit;

    // Reset damage/healing counter
    ResetEffectDamageAndHeal();

    uint8 positiveMask = 0;
    uint8 negativeMask = 0;
    if (missInfo != SPELL_MISS_NONE)
    {
        negativeMask = (mask & m_negativeEffectMask);
        positiveMask = mask & ~m_negativeEffectMask;
    }

    if (negativeMask != 0 && positiveMask != 0)
    {
        //We missed the negative effects but should still apply the positive effects- apply the positive effects now
        //and then we'll show misses for the negative
        DoSpellHitOnUnit(unit,positiveMask);
    } else if (positiveMask != 0)
    {
        //The spell missed but it's all positive results for this target so just hit anyway
        missInfo = SPELL_MISS_NONE;
    }

    // drop proc flags in case target not affected negative effects in negative spell
    // for example caster bonus or animation,
    // except miss case where will assigned PROC_EX_* flags later
    if (((procAttacker | procVictim) & NEGATIVE_TRIGGER_MASK) &&
            !excludedPersistantAreaAura && !(mask & m_negativeEffectMask) && missInfo == SPELL_MISS_NONE)
    {
        procAttacker = PROC_FLAG_NONE;
        procVictim   = PROC_FLAG_NONE;
    }

    if (m_spellInfo->speed > 0)
    {
        // mark effects that were already handled in Spell::HandleDelayedSpellLaunch on spell launch as processed
        for (int32 i = 0; i < MAX_EFFECT_INDEX; ++i)
            if (IsEffectHandledOnDelayedSpellLaunch(m_spellInfo, SpellEffectIndex(i)))
                mask &= ~(1<<i);

        // maybe used in effects that are handled on hit
        m_damage += target->damage;
    }

    if (missInfo==SPELL_MISS_NONE && mask != 0)             // In case spell hit target, do all effect on that target
        DoSpellHitOnUnit(unit, mask);
    else if (missInfo == SPELL_MISS_REFLECT)                // In case spell reflect from target, do all effect on caster (if hit)
    {
        if (target->reflectResult == SPELL_MISS_NONE)       // If reflected spell hit caster -> do all effect on him
        {
            DoSpellHitOnUnit(m_caster, mask, true);
            unitTarget = m_caster;
        }
    }
    else if (target->effectMask != 0)                                     // in 1.12.1 we need explicit miss info
    {
        if (missInfo == SPELL_MISS_MISS || missInfo == SPELL_MISS_RESIST)
        {
            if (m_spellInfo->DmgClass == SPELL_DAMAGE_CLASS_MAGIC)
            {
                missInfo = SPELL_MISS_RESIST;
            } else
            {
                missInfo = SPELL_MISS_MISS;
            }
        }

        if (real_caster)
            real_caster->SendSpellMiss(unit, m_spellInfo->Id, missInfo);

        if(missInfo == SPELL_MISS_MISS || missInfo == SPELL_MISS_RESIST)
        {
            if(real_caster && real_caster != unit)
            {
                // can cause back attack (if detected)
                if (!(m_spellInfo->AttributesEx3 & SPELL_ATTR_EX3_NO_INITIAL_AGGRO) && !IsPositiveSpell(m_spellInfo->Id) &&
                        m_caster->isVisibleForOrDetect(unit, unit, false)
                        && (m_spellInfo->SpellFamilyName != 6 || m_spellInfo->SpellIconID != 1487) &&
                        (m_spellInfo->SpellFamilyName != 7 || m_spellInfo->SpellIconID != 454)) //Extra check for priest spell Mind Soothe and Druid spell Soothe Animal.
                {
                    if (!unit->isInCombat() && unit->GetTypeId() != TYPEID_PLAYER && ((Creature*)unit)->AI())
                        ((Creature*)unit)->AI()->AttackedBy(real_caster);


                    if(strcmp("Holy Nova", *m_spellInfo->SpellName) == 0) // Holy nova adds no threat.
                    {
                        unit->AddThreat(real_caster, 0);
                    }
                    else
                    {
                        unit->AddThreat(real_caster);
                    }

                    unit->SetInCombatWith(real_caster);
                    real_caster->SetInCombatWith(unit);
                }
            }
        }

        if (missInfo == SPELL_MISS_MISS || missInfo == SPELL_MISS_DODGE || missInfo == SPELL_MISS_PARRY)
        {

            //refund cost of power / energy for rogue / warrior / druid on miss
            Powers powerType = Powers(m_spellInfo->powerType);
            if (powerType == POWER_ENERGY && !NeedsComboPoints(m_spellInfo))
                m_caster->ModifyPower(powerType, (int32)m_powerCost*0.8);
        }

    }

    // All calculated do it!
    // Do healing and triggers
    if (m_healing)
    {
        bool crit = real_caster && real_caster->IsSpellCrit(unitTarget, m_spellInfo, m_spellSchoolMask);
        uint32 addhealth = m_healing;
        if (crit)
        {
            procEx |= PROC_EX_CRITICAL_HIT;
            addhealth = caster->SpellCriticalHealingBonus(m_spellInfo, addhealth, NULL);
        }
        else
            procEx |= PROC_EX_NORMAL_HIT;

        // Do triggers for unit (reflect triggers passed on hit phase for correct drop charge)
        if (m_canTrigger && missInfo != SPELL_MISS_REFLECT)
        {
            // Some spell expected send main spell info to triggered system
            SpellEntry const* spellInfo = m_spellInfo;
            switch(m_spellInfo->Id)
            {
            case 19968:                                 // Holy Light triggered heal
            case 19993:                                 // Flash of Light triggered heal
            {
                // stored in unused spell effect basepoints in main spell code
                uint32 spellid = m_currentBasePoints[EFFECT_INDEX_1];
                spellInfo = sSpellStore.LookupEntry(spellid);

                break;
            }
            }

            bool isFirstTarget = true;
            if (m_spellInfo->SpellIconID == 13) // Shaman's Manastream setbonus from Earthfury that shouldn't proc when the spell jumps to additional targets.
            {
                if (unitTarget->GetGUID() != m_UniqueTargetInfo.front().targetGUID)
                    isFirstTarget = false;
            }

            caster->ProcDamageAndSpell(unitTarget, real_caster ? procAttacker : (uint32) PROC_FLAG_NONE, procVictim, procEx, addhealth, m_startedCasting, m_attackType, spellInfo, isFirstTarget);
        }

        int32 gain = caster->DealHeal(unitTarget, addhealth, m_spellInfo, crit);

        if (real_caster && (strcmp("Holy Nova", *m_spellInfo->SpellName) == 0)) // Holy nova adds no threat.
        {
            unitTarget->getHostileRefManager().threatAssist(real_caster, 0, m_spellInfo);
        }
        else
        {
            unitTarget->getHostileRefManager().threatAssist(real_caster, float(gain) * 0.5f * sSpellMgr.GetSpellThreatMultiplier(m_spellInfo), m_spellInfo);
        }

    }
    // Do damage and triggers
    else if (m_damage)
    {
        // Fill base damage struct (unitTarget - is real spell target)
        SpellNonMeleeDamage damageInfo(caster, unitTarget, m_spellInfo->Id, GetFirstSchoolInMask(m_spellSchoolMask));

        if (m_spellInfo->speed > 0)
        {
            damageInfo.damage = m_damage;
            damageInfo.HitInfo = target->HitInfo;
        }
        // Add bonuses and fill damageInfo struct
        else
            caster->CalculateSpellDamage(&damageInfo, m_damage, m_spellInfo, m_attackType);

        unitTarget->CalculateAbsorbResistBlock(caster, &damageInfo, m_spellInfo,BASE_ATTACK,target->partialResist);

        caster->DealDamageMods(damageInfo.target, damageInfo.damage, &damageInfo.absorb);

        // Send log damage message to client
        caster->SendSpellNonMeleeDamageLog(&damageInfo);

        procEx = createProcExtendMask(&damageInfo, missInfo);
        procVictim |= PROC_FLAG_TAKEN_ANY_DAMAGE;



        // Do triggers for unit (reflect triggers passed on hit phase for correct drop charge)
        if (m_canTrigger && missInfo != SPELL_MISS_REFLECT)
            caster->ProcDamageAndSpell(unitTarget, real_caster ? procAttacker : (uint32) PROC_FLAG_NONE, procVictim, procEx, damageInfo.damage, m_startedCasting, m_attackType, m_spellInfo);

        // Okay, screw it- stormstrike doesn't require a weapon but it needs to generate procs.  Are there other abilities like that?  Maybe, but they haven't
        // been reported and I am apparently real bad at identifying them myself.  So for now, just force stormstrike to generate procs, since we know for sure
        // that it should.  If another ability comes up that should be generating procs, we can see what the deal is.

        // trigger weapon enchants for weapon based spells; exclude spells that stop attack, because may break CC
        if (m_caster->GetTypeId() == TYPEID_PLAYER && (m_spellInfo->EquippedItemClass == ITEM_CLASS_WEAPON || m_spellInfo->Id == 17364) &&
                !(m_spellInfo->Attributes & SPELL_ATTR_STOP_ATTACK_TARGET))
        {
            ((Player*)m_caster)->CastItemCombatSpell(unitTarget, m_attackType);
        }

        caster->DealSpellDamage(&damageInfo, true);

        // Bloodthirst
        if (m_spellInfo->SpellFamilyName == SPELLFAMILY_WARRIOR && m_spellInfo->SpellFamilyFlags & UI64LIT(0x0000000002000000))
        {
            uint32 BTAura = 0;
            switch(m_spellInfo->Id)
            {
            case 23881:
                BTAura = 23885;
                break;
            case 23892:
                BTAura = 23886;
                break;
            case 23893:
                BTAura = 23887;
                break;
            case 23894:
                BTAura = 23888;
                break;
            default:
                //Incorrectly sending out error messages for Mortal Strike and Shield Slam
                //sLog.outError("Spell::EffectSchoolDMG: Spell %u not handled in BTAura",m_spellInfo->Id);
                break;
            }
            if (BTAura)
                m_caster->CastSpell(m_caster,BTAura,true);
        }
    }
    // Passive spell hits/misses or active spells only misses (only triggers if proc flags set)
    else if (procAttacker || procVictim)
    {
        // Fill base damage struct (unitTarget - is real spell target)
        SpellNonMeleeDamage damageInfo(caster, unitTarget, m_spellInfo->Id, GetFirstSchoolInMask(m_spellSchoolMask));
        procEx = createProcExtendMask(&damageInfo, missInfo);
        // Do triggers for unit (reflect triggers passed on hit phase for correct drop charge)
        if (m_canTrigger && missInfo != SPELL_MISS_REFLECT)
        {
            //HACK: There's some scenario where we pass-through the aura that triggered us... is itw henever we don't have a valid family?  We know we do it for
            //spell 18350
            if (m_spellInfo->Id == 18350)
                caster->ProcDamageAndSpell(unit, real_caster ? procAttacker : (uint32) PROC_FLAG_NONE, procVictim, procEx, 0, m_startedCasting, m_attackType, m_triggeredByAuraSpell);
            else
                caster->ProcDamageAndSpell(unit, real_caster ? procAttacker : (uint32) PROC_FLAG_NONE, procVictim, procEx, 0, m_startedCasting, m_attackType, m_spellInfo);
        }
    }

    // Call scripted function for AI if this spell is casted upon a creature
    if (unit->GetTypeId() == TYPEID_UNIT)
    {
        // cast at creature (or GO) quest objectives update at successful cast finished (+channel finished)
        // ignore pets or autorepeat/melee casts for speed (not exist quest for spells (hm... )
        if (real_caster && !((Creature*)unit)->IsPet() && !IsAutoRepeat() && !IsNextMeleeSwingSpell() && !IsChannelActive())
            if (Player* p = real_caster->GetCharmerOrOwnerPlayerOrPlayerItself())
                p->RewardPlayerAndGroupAtCast(unit, m_spellInfo->Id);

        if(((Creature*)unit)->AI())
            ((Creature*)unit)->AI()->SpellHit(m_caster, m_spellInfo);
    }

    // Call scripted function for AI if this spell is casted by a creature
    if (m_caster->GetTypeId() == TYPEID_UNIT && ((Creature*)m_caster)->AI())
        ((Creature*)m_caster)->AI()->SpellHitTarget(unit, m_spellInfo);
}

void Spell::DoSpellHitOnUnit(Unit *unit, uint32 effectMask, bool isReflected)
{
    if (!unit || !effectMask)
        return;

    Unit* realCaster = GetAffectiveCaster();

    // Recheck immune (only for delayed spells)
    if (m_spellInfo->speed && (
                unit->IsImmunedToDamage(GetSpellSchoolMask(m_spellInfo)) ||
                unit->IsImmuneToSpell(m_spellInfo)))
    {
        if (realCaster)
            realCaster->SendSpellMiss(unit, m_spellInfo->Id, SPELL_MISS_IMMUNE);

        ResetEffectDamageAndHeal();
        return;
    }

    bool doDiminishingReturns = true;

    if (realCaster && realCaster != unit)
    {
        //Monsters shouldn't run diminishing returns on the player
        if (!realCaster->GetCharmerOrOwnerOrOwnGuid().IsPlayer())
        {
            doDiminishingReturns = false;
        }

        // Recheck  UNIT_FLAG_NON_ATTACKABLE for delayed spells
        if (m_spellInfo->speed > 0.0f &&
                unit->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE) && 
                (unit->GetTypeId() != TYPEID_UNIT || !dynamic_cast<Creature*>(unit)->GetIgnoreNonCombatFlags()) &&
                unit->GetCharmerOrOwnerGuid() != m_caster->GetObjectGuid())
        {
            realCaster->SendSpellMiss(unit, m_spellInfo->Id, SPELL_MISS_EVADE);
            ResetEffectDamageAndHeal();
            return;
        }

        if (!realCaster->IsFriendlyTo(unit))
        {
            // for delayed spells ignore not visible explicit target
            if (m_spellInfo->speed > 0.0f && unit == m_targets.getUnitTarget() &&
                    !unit->isVisibleForOrDetect(m_caster, m_caster, false))
            {
                realCaster->SendSpellMiss(unit, m_spellInfo->Id, SPELL_MISS_EVADE);
                ResetEffectDamageAndHeal();
                return;
            }

            // not break stealth by cast targeting
            if (!(m_spellInfo->AttributesEx & SPELL_ATTR_EX_NOT_BREAK_STEALTH))
                unit->RemoveSpellsCausingAura(SPELL_AURA_MOD_STEALTH);

            // can cause back attack (if detected), stealth removed at Spell::cast if spell break it
            if (!(m_spellInfo->AttributesEx3 & SPELL_ATTR_EX3_NO_INITIAL_AGGRO) && !IsPositiveSpell(m_spellInfo->Id) &&
                    m_caster->isVisibleForOrDetect(unit, unit, false) && (m_spellInfo->SpellFamilyName != 6 || m_spellInfo->SpellIconID != 1487)
                    && (m_spellInfo->SpellFamilyName != 7 || m_spellInfo->SpellIconID != 454)) //Extra check for priest spell Mind Soothe and Druid spell Soothe Animal.
            {
                // use speedup check to avoid re-remove after above lines
                if (m_spellInfo->AttributesEx & SPELL_ATTR_EX_NOT_BREAK_STEALTH)
                    unit->RemoveSpellsCausingAura(SPELL_AURA_MOD_STEALTH);

                // caster can be detected but have stealth aura
                m_caster->RemoveSpellsCausingAura(SPELL_AURA_MOD_STEALTH);

                if (!(m_spellInfo->SpellFamilyName == SPELLFAMILY_ROGUE && (m_spellInfo->SpellFamilyFlags & UI64LIT(0x00000080) || m_spellInfo->SpellFamilyFlags & 2147483648)))
                    m_caster->RemoveSpellsCausingAura(SPELL_AURA_MOD_STEALTH);


                if (!unit->IsStandState() && !unit->hasUnitState(UNIT_STAT_STUNNED))
                    unit->SetStandState(UNIT_STAND_STATE_STAND);

                if (!unit->IsPassiveToSpells())
                {
                    if((m_spellInfo->Id != 11578 && m_spellInfo->Id != 100 && m_spellInfo->Id != 6178 &&
                        m_spellInfo->Id != 20252 && m_spellInfo->Id != 20616 && m_spellInfo->Id != 20617 && 
                        m_spellInfo->Id != 16979 && m_spellInfo->Id != 22641 && m_spellInfo->Id != 25042))
                    {
                        if (!unit->isInCombat() && unit->GetTypeId() != TYPEID_PLAYER && ((Creature*)unit)->AI())
                            ((Creature*)unit)->AI()->AttackedBy(realCaster);
                    }

                    if(strcmp("Holy Nova", *m_spellInfo->SpellName) == 0) // Holy nova adds no threat.
                    {
                        unit->AddThreat(realCaster, 0);
                    }
                    else
                    {
                        if((m_spellInfo->Id != 11578 && m_spellInfo->Id != 100 && m_spellInfo->Id != 6178 && 
                            m_spellInfo->Id != 20252 && m_spellInfo->Id != 20616 && m_spellInfo->Id != 20617 &&
                            m_spellInfo->Id != 16979 && m_spellInfo->Id != 22641 && m_spellInfo->Id != 25042))
                        {
                            unit->AddThreat(realCaster);
                        }
                    }

                    if((m_spellInfo->Id != 11578 && m_spellInfo->Id != 100 && m_spellInfo->Id != 6178 && 
                        m_spellInfo->Id != 20252 && m_spellInfo->Id != 20616 && m_spellInfo->Id != 20617 &&
                        m_spellInfo->Id != 16979 && m_spellInfo->Id != 22641 && m_spellInfo->Id != 25042))
                    {
                        unit->SetInCombatWith(realCaster);
                        realCaster->SetInCombatWith(unit);
                    }
                }

                if (Player *attackedPlayer = unit->GetCharmerOrOwnerPlayerOrPlayerItself())
                    realCaster->SetContestedPvP(attackedPlayer);
            }
        }
        else
        {
            // for delayed spells ignore negative spells (after duel end) for friendly targets
            if (m_spellInfo->speed > 0.0f && !IsPositiveSpell(m_spellInfo->Id))
            {
                realCaster->SendSpellMiss(unit, m_spellInfo->Id, SPELL_MISS_EVADE);
                ResetEffectDamageAndHeal();
                return;
            }

            // assisting case, healing and resurrection
            if (unit->hasUnitState(UNIT_STAT_ATTACK_PLAYER))
                realCaster->SetContestedPvP();

            if (unit->isInCombat() && !(m_spellInfo->AttributesEx3 & SPELL_ATTR_EX3_NO_INITIAL_AGGRO))
            {
                realCaster->SetInCombatState(unit->GetCombatTimer() > 0);
                unit->getHostileRefManager().threatAssist(realCaster, 0.0f, m_spellInfo);
            }
        }
    }

    if (doDiminishingReturns)
    {


        ItemPrototype const *prototype = NULL;  // For special cases with items when it comes to diminishing returns.
        uint32 itemId = 0;
        if (m_CastItem)
        {
            prototype = m_CastItem->GetProto();
            itemId = prototype->ItemId;
        }

        // Get Data Needed for Diminishing Returns, some effects may have multiple auras, so this must be done on spell hit, not aura add
        m_diminishGroup = GetDiminishingReturnsGroupForSpell(m_spellInfo, m_triggeredByAuraSpell, itemId);
        m_diminishLevel = unit->GetDiminishing(m_diminishGroup);

        // Increase Diminishing on unit, current informations for actually casts will use values above
        if ((GetDiminishingReturnsGroupType(m_diminishGroup) == DRTYPE_PLAYER && unit->GetTypeId() == TYPEID_PLAYER) ||
                GetDiminishingReturnsGroupType(m_diminishGroup) == DRTYPE_ALL)
            unit->IncrDiminishing(m_diminishGroup);
    }

    // Apply additional spell effects to target
    CastPreCastSpells(unit);

    if (IsSpellAppliesAura(m_spellInfo, effectMask))
    {
        m_spellAuraHolder = CreateSpellAuraHolder(m_spellInfo, m_IsTriggeredSpell, unit, realCaster, m_CastItem);
        m_spellAuraHolder->setDiminishGroup(m_diminishGroup);
    }
    else
        m_spellAuraHolder = NULL;

    if (unitTarget)
    {
        std::list<Aura*> const& vManaShield = unitTarget->GetAurasByType(SPELL_AURA_MANA_SHIELD);
        std::list<Aura*> const& vSchoolAbsorb = unitTarget->GetAurasByType(SPELL_AURA_SCHOOL_ABSORB);

        if (!vManaShield.empty() || !vSchoolAbsorb.empty())
        {
            // Make sure that CC and other things are interrupted through absorb auras.
            for (int effectNumber = 0; effectNumber < MAX_EFFECT_INDEX; ++effectNumber)
            {
                if (m_spellInfo->Effect[effectNumber] == SPELL_EFFECT_SCHOOL_DAMAGE ||
                     m_spellInfo->Effect[effectNumber] == SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL ||
                     m_spellInfo->Effect[effectNumber] == SPELL_EFFECT_WEAPON_PERCENT_DAMAGE ||
                     m_spellInfo->Effect[effectNumber] == SPELL_EFFECT_WEAPON_DAMAGE ||
                     m_spellInfo->Effect[effectNumber] == SPELL_EFFECT_NORMALIZED_WEAPON_DMG)
                {
                    unitTarget->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_DAMAGE);
                    break;
                }

            }
        }
    }

    for(int effectNumber = 0; effectNumber < MAX_EFFECT_INDEX; ++effectNumber)
    {
        if (effectMask & (1 << effectNumber))
        {
            HandleEffects(unit, NULL, NULL, SpellEffectIndex(effectNumber), m_damageMultipliers[effectNumber]);
            if ( m_applyMultiplierMask & (1 << effectNumber) )
            {
                // Get multiplier
                float multiplier = m_spellInfo->DmgMultiplier[effectNumber];
                // Apply multiplier mods
                if (realCaster)
                    if(Player* modOwner = realCaster->GetSpellModOwner())
                        modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_EFFECT_PAST_FIRST, multiplier, this);
                m_damageMultipliers[effectNumber] *= multiplier;
            }
        }
    }

    // now apply all created auras
    if (m_spellAuraHolder)
    {
        // normally shouldn't happen
        if (!m_spellAuraHolder->IsEmptyHolder())
        {
            int32 duration = m_spellAuraHolder->GetAuraMaxDuration();
            int32 originalDuration = duration;

            if (duration > 0 && doDiminishingReturns)
            {
                unit->ApplyDiminishingToDuration(m_diminishGroup, duration, m_caster, m_diminishLevel, isReflected);

                // Fully diminished
                if (duration == 0)
                {
                    delete m_spellAuraHolder;
                    return;
                }
            }

            if (duration != originalDuration)
            {
                m_spellAuraHolder->SetAuraMaxDuration(duration);
                m_spellAuraHolder->SetAuraDuration(duration);
            }

            Unit::AuraList const* healingList = &unit->GetAurasByType(AuraType(m_spellInfo->EffectApplyAuraName[1]));

            // Make sure the HoT-part of Regrowth does not stack.
            if (m_spellInfo && strcmp("Regrowth", *m_spellInfo->SpellName) == 0 && healingList)
            {
                bool match = true;

                if (!healingList)
                    return;

                uint32 m_healingValue = realCaster->CalculateSpellDamage(unit, m_spellInfo,(SpellEffectIndex) 1, m_spellInfo->EffectBasePoints + 1);
                m_healingValue = realCaster->SpellHealingBonusDone(unit, m_spellInfo, m_healingValue, DOT, 1) + 1;


                for (Aura* healingAura : *healingList)
                {
                    Modifier *m_healingModifier = healingAura->GetModifier();
                    SpellEntry const* healingSpellproto = healingAura->GetSpellProto();

                    int spellNameComparison = strcmp((const char*) healingSpellproto->SpellName, (const char*) m_spellInfo->SpellName);

                    if ((uint32) m_healingModifier->m_amount > m_healingValue && spellNameComparison == 0 ) // If a currently existing HoT is stronger than the attempted casted one we do not apply the new HoT.
                    {
                        match = false;
                        break;
                    }
                    else if ((uint32) m_healingModifier->m_amount <= m_healingValue && spellNameComparison == 0) // If the currently existing HoT is weaker than the attempted one we replace the current HoT.
                    {
                        break;
                    }
                    else
                        break;
                }


                if (match)
                {
                    unit->RemoveAura(m_spellInfo->Id, (SpellEffectIndex) 1);
                    unit->AddSpellAuraHolder(m_spellAuraHolder);
                    return;
                }
                else
                {
                    delete m_spellAuraHolder;
                    return;
                }

            }

            unit->AddSpellAuraHolder(m_spellAuraHolder);
        }
        else
            delete m_spellAuraHolder;
    }
}

void Spell::DoAllEffectOnTarget(GOTargetInfo *target)
{
    if (target->processed)                                  // Check target
        return;
    target->processed = true;                               // Target checked in apply effects procedure

    uint32 effectMask = target->effectMask;
    if(!effectMask)
        return;

    GameObject* go = m_caster->GetMap()->GetGameObject(target->targetGUID);
    if(!go)
        return;

    for(int effectNumber = 0; effectNumber < MAX_EFFECT_INDEX; ++effectNumber)
        if (effectMask & (1 << effectNumber))
            HandleEffects(NULL, NULL, go, SpellEffectIndex(effectNumber));

    // cast at creature (or GO) quest objectives update at successful cast finished (+channel finished)
    // ignore autorepeat/melee casts for speed (not exist quest for spells (hm... )
    if( !IsAutoRepeat() && !IsNextMeleeSwingSpell() && !IsChannelActive() )
    {
        if ( Player* p = m_caster->GetCharmerOrOwnerPlayerOrPlayerItself() )
            p->RewardPlayerAndGroupAtCast(go, m_spellInfo->Id);
    }
}

void Spell::DoAllEffectOnTarget(ItemTargetInfo *target)
{
    uint32 effectMask = target->effectMask;
    if(!target->item || !effectMask)
        return;

    for(int effectNumber = 0; effectNumber < MAX_EFFECT_INDEX; ++effectNumber)
        if (effectMask & (1 << effectNumber))
            HandleEffects(NULL, target->item, NULL, SpellEffectIndex(effectNumber));
}

void Spell::HandleDelayedSpellLaunch(TargetInfo *target)
{
    // Get mask of effects for target
    uint32 mask = target->effectMask;

    Unit* unit = m_caster->GetObjectGuid() == target->targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, target->targetGUID);
    if (!unit)
        return;

    // Get original caster (if exist) and calculate damage/healing from him data
    Unit *real_caster = GetAffectiveCaster();
    // FIXME: in case wild GO heal/damage spells will be used target bonuses
    Unit *caster = real_caster ? real_caster : m_caster;

    SpellMissInfo missInfo = target->missCondition;
    // Need init unitTarget by default unit (can changed in code on reflect)
    // Or on missInfo!=SPELL_MISS_NONE unitTarget undefined (but need in trigger subsystem)
    unitTarget = unit;

    // Reset damage/healing counter
    m_damage = 0;
    m_healing = 0; // healing maybe not needed at this point

    // Fill base damage struct (unitTarget - is real spell target)
    SpellNonMeleeDamage damageInfo(caster, unitTarget, m_spellInfo->Id, GetFirstSchoolInMask(m_spellSchoolMask));

    // keep damage amount for reflected spells
    if (missInfo == SPELL_MISS_NONE || (missInfo == SPELL_MISS_REFLECT && target->reflectResult == SPELL_MISS_NONE))
    {
        for (int32 effectNumber = 0; effectNumber < MAX_EFFECT_INDEX; ++effectNumber)
        {
            if (mask & (1 << effectNumber) && IsEffectHandledOnDelayedSpellLaunch(m_spellInfo, SpellEffectIndex(effectNumber)))
            {
                HandleEffects(unit, NULL, NULL, SpellEffectIndex(effectNumber), m_damageMultipliers[effectNumber]);
                if ( m_applyMultiplierMask & (1 << effectNumber) )
                {
                    // Get multiplier
                    float multiplier = m_spellInfo->DmgMultiplier[effectNumber];
                    // Apply multiplier mods
                    if (real_caster)
                        if(Player* modOwner = real_caster->GetSpellModOwner())
                            modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_EFFECT_PAST_FIRST, multiplier, this);
                    m_damageMultipliers[effectNumber] *= multiplier;
                }
            }
        }

        if (m_damage > 0)
            caster->CalculateSpellDamage(&damageInfo, m_damage, m_spellInfo, m_attackType);
    }

    target->damage = damageInfo.damage;
    target->HitInfo = damageInfo.HitInfo;
}

void Spell::InitializeDamageMultipliers()
{
    for (int32 i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        if (m_spellInfo->Effect[i] == 0)
            continue;

        uint32 EffectChainTarget = m_spellInfo->EffectChainTarget[i];
        if (Unit* realCaster = GetAffectiveCaster())
            if(Player* modOwner = realCaster->GetSpellModOwner())
                modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_JUMP_TARGETS, EffectChainTarget, this);

        m_damageMultipliers[i] = 1.0f;
        if( (m_spellInfo->EffectImplicitTargetA[i] == TARGET_CHAIN_DAMAGE || m_spellInfo->EffectImplicitTargetA[i] == TARGET_CHAIN_HEAL) &&
                (EffectChainTarget > 1) )
            m_applyMultiplierMask |= (1 << i);
    }
}

bool Spell::IsAliveUnitPresentInTargetList()
{
    // Not need check return true
    if (m_needAliveTargetMask == 0)
        return true;

    uint8 needAliveTargetMask = m_needAliveTargetMask;

    for(TargetList::const_iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        if (ihit->missCondition == SPELL_MISS_NONE && (needAliveTargetMask & ihit->effectMask))
        {
            Unit *unit = m_caster->GetObjectGuid() == ihit->targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, ihit->targetGUID);

            // either unit is alive and normal spell, or unit dead and deathonly-spell
            if (unit && (unit->isAlive() != IsDeathOnlySpell(m_spellInfo)))
                needAliveTargetMask &= ~ihit->effectMask;   // remove from need alive mask effect that have alive target
        }
    }

    // is all effects from m_needAliveTargetMask have alive targets
    return needAliveTargetMask == 0;
}

// Helper for Chain Healing
// Spell target first
// Raidmates then descending by injury suffered (MaxHealth - Health)
// Other players/mobs then descending by injury suffered (MaxHealth - Health)
struct ChainHealingOrder : public std::binary_function<const Unit*, const Unit*, bool>
{
    const Unit* MainTarget;
    ChainHealingOrder(Unit const* Target) : MainTarget(Target) {};
    // functor for operator ">"
    bool operator()(Unit const* _Left, Unit const* _Right) const
    {
        return (ChainHealingHash(_Left) < ChainHealingHash(_Right));
    }
    int32 ChainHealingHash(Unit const* Target) const
    {
        if (Target == MainTarget)
            return 0;
        else if (Target->GetTypeId() == TYPEID_PLAYER && MainTarget->GetTypeId() == TYPEID_PLAYER &&
                 ((Player const*)Target)->IsInSameRaidWith((Player const*)MainTarget))
        {
            if (Target->GetHealth() == Target->GetMaxHealth())
                return 40000;
            else
                return 20000 - Target->GetMaxHealth() + Target->GetHealth();
        }
        else
            return 40000 - Target->GetMaxHealth() + Target->GetHealth();
    }
};

class ChainHealingFullHealth: std::unary_function<const Unit*, bool>
{
public:
    const Unit* MainTarget;
    ChainHealingFullHealth(const Unit* Target) : MainTarget(Target) {};

    bool operator()(const Unit* Target)
    {
        return (Target != MainTarget && Target->GetHealth() == Target->GetMaxHealth());
    }
};

// Helper for targets nearest to the spell target
// The spell target is always first unless there is a target at _completely_ the same position (unbelievable case)
struct TargetDistanceOrderNear : public std::binary_function<const Unit, const Unit, bool>
{
    const Unit* MainTarget;
    TargetDistanceOrderNear(const Unit* Target) : MainTarget(Target) {};
    // functor for operator ">"
    bool operator()(const Unit* _Left, const Unit* _Right) const
    {
        return MainTarget->GetDistanceOrder(_Left, _Right);
    }
};

void Spell::SetTargetMap(SpellEffectIndex effIndex, uint32 targetMode, UnitList& targetUnitMap)
{
    float radius;
    if (m_spellInfo->EffectRadiusIndex[effIndex])
        radius = GetSpellRadius(sSpellRadiusStore.LookupEntry(m_spellInfo->EffectRadiusIndex[effIndex]));
    else
        radius = GetSpellMaxRange(sSpellRangeStore.LookupEntry(m_spellInfo->rangeIndex));

    uint32 EffectChainTarget = m_spellInfo->EffectChainTarget[effIndex];

    if (Unit* realCaster = GetAffectiveCaster())
    {
        if(Player* modOwner = realCaster->GetSpellModOwner())
        {
            modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_RADIUS, radius, this);
            modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_JUMP_TARGETS, EffectChainTarget, this);
        }
    }

    uint32 unMaxTargets = m_spellInfo->MaxAffectedTargets;

    // custom target amount cases
    switch(m_spellInfo->SpellFamilyName)
    {
    case SPELLFAMILY_GENERIC:
    {
        switch(m_spellInfo->Id)
        {
        case 802:                                   // Mutate Bug
        case 804:                                   // Explode Bug
        case 23138:                                 // Gate of Shazzrah
        case 28560:                                 // Summon Blizzard
            unMaxTargets = 1;
            break;
        case 28542:                                 // Life Drain
            unMaxTargets = 2;
            break;
        case 28796:                                 // Poison Bolt Volley
            unMaxTargets = 10;
            break;
        case 26052:                                 // Poison Bolt Volley (Pincess Huhuran)
            unMaxTargets = 60;
            break;
        case 26180:
            unMaxTargets = 60;
            break;
        }
        break;
    }
    case SPELLFAMILY_HUNTER:
    {
        switch (m_spellInfo->Id)
        {
        case 13810:	//Frost trap needs to center itself around the trap, not the target
        {
            WorldObject *caster = this->GetAffectiveCasterObject();
            if (caster != NULL)
            {
                this->m_targets.setDestination(caster->GetPositionX(),caster->GetPositionY(),caster->GetPositionZ());
            }
        }
        break;
        }
        break;
    }
    default:
        break;
    }

    switch(targetMode)
    {
    case TARGET_TOTEM_EARTH:
    case TARGET_TOTEM_WATER:
    case TARGET_TOTEM_AIR:
    case TARGET_TOTEM_FIRE:
    case TARGET_SELF:
        targetUnitMap.push_back(m_caster);
        break;
    case TARGET_RANDOM_ENEMY_CHAIN_IN_AREA:
    {
        m_targets.m_targetMask = 0;
        unMaxTargets = EffectChainTarget;
        float max_range = radius + unMaxTargets * CHAIN_SPELL_JUMP_RADIUS;

        UnitList tempTargetUnitMap;

        {
            MaNGOS::AnyAoETargetUnitInObjectRangeCheck u_check(m_caster, max_range);
            MaNGOS::UnitListSearcher<MaNGOS::AnyAoETargetUnitInObjectRangeCheck> searcher(tempTargetUnitMap, u_check);
            Cell::VisitAllObjects(m_caster, searcher, max_range);
        }

        if(tempTargetUnitMap.empty())
            break;

        tempTargetUnitMap.sort(TargetDistanceOrderNear(m_caster));

        //Now to get us a random target that's in the initial range of the spell
        uint32 t = 0;
        UnitList::iterator itr = tempTargetUnitMap.begin();
        while(itr!= tempTargetUnitMap.end() && (*itr)->IsWithinDist(m_caster, radius))
            ++t, ++itr;

        if(!t)
            break;

        itr = tempTargetUnitMap.begin();
        std::advance(itr, rand() % t);
        Unit *pUnitTarget = *itr;
        targetUnitMap.push_back(pUnitTarget);

        tempTargetUnitMap.erase(itr);

        tempTargetUnitMap.sort(TargetDistanceOrderNear(pUnitTarget));

        t = unMaxTargets - 1;
        Unit *prev = pUnitTarget;
        UnitList::iterator next = tempTargetUnitMap.begin();

        while(t && next != tempTargetUnitMap.end())
        {
            if(!prev->IsWithinDist(*next, CHAIN_SPELL_JUMP_RADIUS))
                break;

            if(!prev->IsWithinLOSInMap(*next))
            {
                ++next;
                continue;
            }

            prev = *next;
            targetUnitMap.push_back(prev);
            tempTargetUnitMap.erase(next);
            tempTargetUnitMap.sort(TargetDistanceOrderNear(prev));
            next = tempTargetUnitMap.begin();

            --t;
        }
        break;
    }
    case TARGET_RANDOM_FRIEND_CHAIN_IN_AREA:
    {
        m_targets.m_targetMask = 0;
        unMaxTargets = EffectChainTarget;
        float max_range = radius + unMaxTargets * CHAIN_SPELL_JUMP_RADIUS;
        UnitList tempTargetUnitMap;
        {
            MaNGOS::AnyFriendlyUnitInObjectRangeCheck u_check(m_caster, max_range);
            MaNGOS::UnitListSearcher<MaNGOS::AnyFriendlyUnitInObjectRangeCheck> searcher(tempTargetUnitMap, u_check);
            Cell::VisitAllObjects(m_caster, searcher, max_range);
        }

        if(tempTargetUnitMap.empty())
            break;

        tempTargetUnitMap.sort(TargetDistanceOrderNear(m_caster));

        //Now to get us a random target that's in the initial range of the spell
        uint32 t = 0;
        UnitList::iterator itr = tempTargetUnitMap.begin();
        while(itr != tempTargetUnitMap.end() && (*itr)->IsWithinDist(m_caster, radius))
            ++t, ++itr;

        if(!t)
            break;

        itr = tempTargetUnitMap.begin();
        std::advance(itr, rand() % t);
        Unit *pUnitTarget = *itr;
        targetUnitMap.push_back(pUnitTarget);

        tempTargetUnitMap.erase(itr);

        tempTargetUnitMap.sort(TargetDistanceOrderNear(pUnitTarget));

        t = unMaxTargets - 1;
        Unit *prev = pUnitTarget;
        UnitList::iterator next = tempTargetUnitMap.begin();

        while(t && next != tempTargetUnitMap.end())
        {
            if(!prev->IsWithinDist(*next, CHAIN_SPELL_JUMP_RADIUS))
                break;

            if(!prev->IsWithinLOSInMap(*next))
            {
                ++next;
                continue;
            }
            prev = *next;
            targetUnitMap.push_back(prev);
            tempTargetUnitMap.erase(next);
            tempTargetUnitMap.sort(TargetDistanceOrderNear(prev));
            next = tempTargetUnitMap.begin();
            --t;
        }
        break;
    }
    case TARGET_PET:
    {
        Pet* tmpUnit = m_caster->GetPet();
        if (!tmpUnit) break;
        targetUnitMap.push_back(tmpUnit);
        break;
    }
    case TARGET_CHAIN_DAMAGE:
    {
        if (EffectChainTarget <= 1)
        {
            if(Unit* pUnitTarget = m_caster->SelectMagnetTarget(m_targets.getUnitTarget(), this, effIndex))
            {
                m_targets.setUnitTarget(pUnitTarget);
                targetUnitMap.push_back(pUnitTarget);
            }
        }
        else
        {
            Unit* pUnitTarget = m_targets.getUnitTarget();
            WorldObject* originalCaster = GetAffectiveCasterObject();
            if(!pUnitTarget || !originalCaster)
                break;

            unMaxTargets = EffectChainTarget;

            float max_range;
            if(m_spellInfo->DmgClass == SPELL_DAMAGE_CLASS_MELEE)
                max_range = radius;
            else
                //FIXME: This very like horrible hack and wrong for most spells
                max_range = radius + unMaxTargets * CHAIN_SPELL_JUMP_RADIUS;

            UnitList tempTargetUnitMap;

            {
                MaNGOS::AnyAoEVisibleTargetUnitInObjectRangeCheck u_check(pUnitTarget, originalCaster, max_range);
                MaNGOS::UnitListSearcher<MaNGOS::AnyAoEVisibleTargetUnitInObjectRangeCheck> searcher(tempTargetUnitMap, u_check);
                Cell::VisitAllObjects(m_caster, searcher, max_range);
            }

            tempTargetUnitMap.sort(TargetDistanceOrderNear(pUnitTarget));

            if(tempTargetUnitMap.empty())
                break;

            if(*tempTargetUnitMap.begin() == pUnitTarget)
                tempTargetUnitMap.erase(tempTargetUnitMap.begin());

            targetUnitMap.push_back(pUnitTarget);
            uint32 t = unMaxTargets - 1;
            Unit *prev = pUnitTarget;
            UnitList::iterator next = tempTargetUnitMap.begin();

            while(t && next != tempTargetUnitMap.end() )
            {
                if(!prev->IsWithinDist(*next,CHAIN_SPELL_JUMP_RADIUS))
                    break;

                if(!prev->IsWithinLOSInMap(*next))
                {
                    ++next;
                    continue;
                }
                //Cleave: Target needs to be in front of Caster!
                if (m_spellInfo->SpellVisual == 219 && m_spellInfo->SpellIconID == 277 && !m_caster->HasInArc(M_PI_F,*next))
                {
                    ++next;
                    continue;
                }

                prev = *next;
                targetUnitMap.push_back(prev);
                tempTargetUnitMap.erase(next);
                tempTargetUnitMap.sort(TargetDistanceOrderNear(prev));
                next = tempTargetUnitMap.begin();

                --t;
            }
        }
        break;
    }
    case TARGET_ALL_ENEMY_IN_AREA:
    {
        //Custom Range for some spells
        switch (m_spellInfo->Id)
        {
        case 19659:			// Ignite Mana
            radius = 52.0f;
            break;
        case 28599:			// Shadowbolt Volley
            radius = 60.0f;
            break;
        case 23410:                 // Nefarian's class calls.
        case 23397:
        case 23398:
        case 23401:
        case 23418:
        case 23425:
        case 23427:
        case 23436:
        case 23414:
            radius = 300.f;
            break;
        default:
            break;
        }
        FillAreaTargets(targetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_AOE_DAMAGE);

        // To only target the correct classes at Nefarian in BWL.
        uint32 classId = 0;
        switch (m_spellInfo->Id)
        {
        case 23410:			// Nefarian's class calls.
            classId = CLASS_MAGE;
            break;
        case 23397:
            classId = CLASS_WARRIOR;
            break;
        case 23398:
            classId = CLASS_DRUID;
            break;
        case 23401:
            classId = CLASS_PRIEST;
            break;
        case 23418:
            classId = CLASS_PALADIN;
            break;
        case 23425:
            classId = CLASS_SHAMAN;
            break;
        case 23427:
            classId = CLASS_WARLOCK;
            break;
        case 23436:
            classId = CLASS_HUNTER;
            break;
        case 23414:
            classId = CLASS_ROGUE;
            break;

        }

        if (classId)
        {
            auto itr = targetUnitMap.begin();
            while(itr != targetUnitMap.end())								// Loop through the target list and remove all targets with the incorrect class.
            {
                Player* current_player = dynamic_cast<Player*>(*itr);
                if (current_player && current_player->getClass() != classId)
                    itr = targetUnitMap.erase(itr);
                else if (!current_player)
                    itr = targetUnitMap.erase(itr);						// Remove non-player targets.
                else
                    ++itr;
            }
        }

        // Periodic Shadow Storm in AQ40 should not target units
        // that are closer than 5 yd.
        if (m_spellInfo->Id == 26546)
        {
            Unit* pCaster = GetCaster();
            if (pCaster)
            {
                auto itr = targetUnitMap.begin();
                while (itr != targetUnitMap.end())
                {
                    if (pCaster->IsWithinDistInMap(*itr, 5.f, false))
                        itr = targetUnitMap.erase(itr);
                    else
                        ++itr;
                }
            }
        }

        // Princess Huhuran poison bolts should only target the 15 closest units.
        // Princess Huhuran Vywern Sting should only target the 10 closest units.
        if(m_spellInfo->Id == 26052 || m_spellInfo->Id == 26180)
        {
            Unit *pCaster = GetCaster();
            if(pCaster)
            {
                targetUnitMap.sort(TargetDistanceOrderNear(pCaster));
                auto itr = targetUnitMap.begin();
                uint8 itrC;
                
                if(m_spellInfo->Id == 26052)
                    itrC = 1;
                else
                    itrC = 6;

                while (itr != targetUnitMap.end())
                {
                    if (itrC > 15)
                        itr = targetUnitMap.erase(itr);
                    else
                    {
                        ++itr;
                        ++itrC;
                    }
                }
            }
        }


        break;
    }
    case TARGET_AREAEFFECT_INSTANT:
    {
        SpellTargets targetB = SPELL_TARGETS_AOE_DAMAGE;
        
        // Select friendly targets for positive effect
        if (IsPositiveEffect(m_spellInfo, effIndex))
            targetB = SPELL_TARGETS_FRIENDLY;

        // The Explode/Mumate Bug spell in AQ should get all close
        // targets. They are filtered further down.
        if (m_spellInfo->Id == 804 || m_spellInfo->Id == 802)
            targetB = SPELL_TARGETS_ALL;

        UnitList tempTargetUnitMap;
        SpellScriptTargetBounds bounds = sSpellMgr.GetSpellScriptTargetBounds(m_spellInfo->Id);

        // fill real target list if no spell script target defined
        FillAreaTargets(bounds.first != bounds.second ? tempTargetUnitMap : targetUnitMap,
                        radius, PUSH_DEST_CENTER, bounds.first != bounds.second ? SPELL_TARGETS_ALL : targetB);

        if (!tempTargetUnitMap.empty())
        {
            for (UnitList::const_iterator iter = tempTargetUnitMap.begin(); iter != tempTargetUnitMap.end(); ++iter)
            {
                if ((*iter)->GetTypeId() != TYPEID_UNIT)
                    continue;

                for(SpellScriptTarget::const_iterator i_spellST = bounds.first; i_spellST != bounds.second; ++i_spellST)
                {
                    // only creature entries supported for this target type
                    if (i_spellST->second.type == SPELL_TARGET_TYPE_GAMEOBJECT)
                        continue;

                    if ((*iter)->GetEntry() == i_spellST->second.targetEntry)
                    {
                        if (i_spellST->second.type == SPELL_TARGET_TYPE_DEAD && ((Creature*)(*iter))->IsCorpse())
                        {
                            targetUnitMap.push_back((*iter));
                        }
                        else if (i_spellST->second.type == SPELL_TARGET_TYPE_CREATURE && (*iter)->isAlive())
                        {
                            targetUnitMap.push_back((*iter));
                        }

                        break;
                    }
                }
            }
        }

        // Loop though the target list for the spell Explode/Mutate Bug in AQ40
        // and remove any creature that isn't a bug.
        if (!targetUnitMap.empty() && (m_spellInfo->Id == 804 || m_spellInfo->Id == 802))
        {
            // Mutate Bug should target the scorpions and Explode Bug the scarabs.
            uint32 filterCreature = m_spellInfo->Id == 804 ? 15316 : 15317;
            auto itr = targetUnitMap.begin();
            do
            {
                Creature* pCreature = dynamic_cast<Creature*>(*itr);
                if (!pCreature || pCreature->isDead() || pCreature->HasAura(m_spellInfo->Id) ||
                    pCreature->GetEntry() != filterCreature)
                {
                    itr = tempTargetUnitMap.erase(itr);
                }
                else
                    ++itr;
            } while (itr != targetUnitMap.end());


            if (m_caster && !targetUnitMap.empty())
            {
                Unit* pTarget = targetUnitMap.front();
                float dist = m_caster->GetDistance(pTarget);

                for (Unit* currTarget : targetUnitMap)
                {
                    if (m_caster->GetDistance(currTarget) < dist)
                    {
                        dist = m_caster->GetDistance(currTarget);
                        pTarget = currTarget;
                    }
                }

                targetUnitMap.clear();
                targetUnitMap.push_back(pTarget);
            }
        }

        // exclude caster
        targetUnitMap.remove(m_caster);
        break;
    }
    case TARGET_AREAEFFECT_CUSTOM:
    {
        if (m_spellInfo->Effect[effIndex] == SPELL_EFFECT_PERSISTENT_AREA_AURA)
            break;
        else if (m_spellInfo->Effect[effIndex] == SPELL_EFFECT_SUMMON)
        {
            targetUnitMap.push_back(m_caster);
            break;
        }

        UnitList tempTargetUnitMap;
        SpellScriptTargetBounds bounds = sSpellMgr.GetSpellScriptTargetBounds(m_spellInfo->Id);
        // fill real target list if no spell script target defined
        FillAreaTargets(bounds.first != bounds.second ? tempTargetUnitMap : targetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_ALL);

        if (!tempTargetUnitMap.empty())
        {
            for (UnitList::const_iterator iter = tempTargetUnitMap.begin(); iter != tempTargetUnitMap.end(); ++iter)
            {
                if ((*iter)->GetTypeId() != TYPEID_UNIT)
                    continue;

                for(SpellScriptTarget::const_iterator i_spellST = bounds.first; i_spellST != bounds.second; ++i_spellST)
                {
                    // only creature entries supported for this target type
                    if (i_spellST->second.type == SPELL_TARGET_TYPE_GAMEOBJECT)
                        continue;

                    if ((*iter)->GetEntry() == i_spellST->second.targetEntry)
                    {
                        if (i_spellST->second.type == SPELL_TARGET_TYPE_DEAD && ((Creature*)(*iter))->IsCorpse())
                        {
                            targetUnitMap.push_back((*iter));
                        }
                        else if (i_spellST->second.type == SPELL_TARGET_TYPE_CREATURE && (*iter)->isAlive())
                        {
                            targetUnitMap.push_back((*iter));
                        }

                        break;
                    }
                }
            }
        }
        else
        {
            // remove not targetable units if spell has no script targets
            for (UnitList::iterator itr = targetUnitMap.begin(); itr != targetUnitMap.end(); )
            {
                if (!(*itr)->isTargetableForAttack(m_spellInfo->AttributesEx3 & SPELL_ATTR_EX3_CAST_ON_DEAD))
                    targetUnitMap.erase(itr++);
                else
                    ++itr;
            }
        }
        break;
    }
    case TARGET_AREAEFFECT_GO_AROUND_DEST:
    {
        // It may be possible to fill targets for some spell effects
        // automatically (SPELL_EFFECT_WMO_REPAIR(88) for example) but
        // for some/most spells we clearly need/want to limit with spell_target_script

        // Some spells untested, for affected GO type 33. May need further adjustments for spells related.

        SpellScriptTargetBounds bounds = sSpellMgr.GetSpellScriptTargetBounds(m_spellInfo->Id);

        std::list<GameObject*> tempTargetGOList;

        for(SpellScriptTarget::const_iterator i_spellST = bounds.first; i_spellST != bounds.second; ++i_spellST)
        {
            if (i_spellST->second.type == SPELL_TARGET_TYPE_GAMEOBJECT)
            {
                // search all GO's with entry, within range of m_destN
                MaNGOS::GameObjectEntryInPosRangeCheck go_check(*m_caster, i_spellST->second.targetEntry, m_targets.m_destX, m_targets.m_destY, m_targets.m_destZ, radius);
                MaNGOS::GameObjectListSearcher<MaNGOS::GameObjectEntryInPosRangeCheck> checker(tempTargetGOList, go_check);
                Cell::VisitGridObjects(m_targets.m_destX,m_targets.m_destY,m_caster->GetMap(), checker, radius*2);
            }
        }

        if (!tempTargetGOList.empty())
        {
            for(std::list<GameObject*>::iterator iter = tempTargetGOList.begin(); iter != tempTargetGOList.end(); ++iter)
                AddGOTarget(*iter, effIndex);
        }

        break;
    }
    case TARGET_ALL_ENEMY_IN_AREA_INSTANT:
    {
        // targets the ground, not the units in the area
        switch(m_spellInfo->Effect[effIndex])
        {
        case SPELL_EFFECT_PERSISTENT_AREA_AURA:
            break;
        case SPELL_EFFECT_SUMMON:
            targetUnitMap.push_back(m_caster);
            break;
        default:
            FillAreaTargets(targetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_AOE_DAMAGE);
            break;
        }
        break;
    }
    case TARGET_DUELVSPLAYER_COORDINATES:
    {
        if(Unit* currentTarget = m_targets.getUnitTarget())
            m_targets.setDestination(currentTarget->GetPositionX(), currentTarget->GetPositionY(), currentTarget->GetPositionZ());
        break;
    }
    case TARGET_ALL_PARTY_AROUND_CASTER:
    case TARGET_ALL_PARTY_AROUND_CASTER_2:
    case TARGET_ALL_PARTY:
        FillRaidOrPartyTargets(targetUnitMap, m_caster, radius, false, true, true);
        break;
    case TARGET_ALL_RAID_AROUND_CASTER:
        FillRaidOrPartyTargets(targetUnitMap, m_caster, radius, true, true, false);
        break;
    case TARGET_SINGLE_FRIEND:
    case TARGET_SINGLE_FRIEND_2:
        if(m_targets.getUnitTarget())
            targetUnitMap.push_back(m_targets.getUnitTarget());
        break;
    case TARGET_CASTER_COORDINATES:
    {
        // Check original caster is GO - set its coordinates as dst cast
        if (WorldObject *caster = GetCastingObject())
            m_targets.setDestination(caster->GetPositionX(), caster->GetPositionY(), caster->GetPositionZ());
        break;
    }
    case TARGET_ALL_HOSTILE_UNITS_AROUND_CASTER:
        FillAreaTargets(targetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_HOSTILE);
        break;
    case TARGET_ALL_FRIENDLY_UNITS_AROUND_CASTER:
        // selected friendly units (for casting objects) around casting object. A friendly unit should be interpreted as a not hostile unit.
        FillAreaTargets(targetUnitMap, radius, PUSH_SELF_CENTER, SPELL_TARGETS_NOT_HOSTILE, GetCastingObject());
        break;
    case TARGET_ALL_FRIENDLY_UNITS_IN_AREA:
        FillAreaTargets(targetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_FRIENDLY);
        break;
        // TARGET_SINGLE_PARTY means that the spells can only be casted on a party member and not on the caster (some seals, fire shield from imp, etc..)
    case TARGET_SINGLE_PARTY:
    {
        Unit *target = m_targets.getUnitTarget();
        // Those spells apparently can't be casted on the caster.
        if( target && target != m_caster)
        {
            // Can only be casted on group's members or its pets
            Group  *pGroup = NULL;

            Unit* owner = m_caster->GetCharmerOrOwner();
            Unit *targetOwner = target->GetCharmerOrOwner();
            if(owner)
            {
                if(owner->GetTypeId() == TYPEID_PLAYER)
                {
                    if( target == owner )
                    {
                        targetUnitMap.push_back(target);
                        break;
                    }
                    pGroup = ((Player*)owner)->GetGroup();
                }
            }
            else if (m_caster->GetTypeId() == TYPEID_PLAYER)
            {
                if( targetOwner == m_caster && target->GetTypeId() == TYPEID_UNIT && ((Creature*)target)->IsPet())
                {
                    targetUnitMap.push_back(target);
                    break;
                }
                pGroup = ((Player*)m_caster)->GetGroup();
            }

            if(pGroup)
            {
                // Our target can also be a player's pet who's grouped with us or our pet. But can't be controlled player
                if(targetOwner)
                {
                    if( targetOwner->GetTypeId() == TYPEID_PLAYER &&
                            target->GetTypeId() == TYPEID_UNIT && (((Creature*)target)->IsPet()) &&
                            target->GetOwnerGuid() == targetOwner->GetObjectGuid() &&
                            pGroup->IsMember(((Player*)targetOwner)->GetObjectGuid()))
                    {
                        targetUnitMap.push_back(target);
                    }
                }
                // 1Our target can be a player who is on our group
                else if (target->GetTypeId() == TYPEID_PLAYER && pGroup->IsMember(((Player*)target)->GetObjectGuid()))
                {
                    targetUnitMap.push_back(target);
                }
            }
        }
        break;
    }
    case TARGET_GAMEOBJECT:
        if(m_targets.getGOTarget())
            AddGOTarget(m_targets.getGOTarget(), effIndex);
        break;
    case TARGET_IN_FRONT_OF_CASTER:
    {
        bool inFront = m_spellInfo->SpellVisual != 3879;
        FillAreaTargets(targetUnitMap, radius, inFront ? PUSH_IN_FRONT : PUSH_IN_BACK, SPELL_TARGETS_AOE_DAMAGE);
        break;
    }
    case TARGET_LARGE_FRONTAL_CONE:
        FillAreaTargets(targetUnitMap, radius, PUSH_IN_FRONT_LARGE, SPELL_TARGETS_AOE_DAMAGE);
        break;
    case TARGET_NARROW_FRONTAL_CONE:
        FillAreaTargets(targetUnitMap, radius, PUSH_IN_FRONT_15, SPELL_TARGETS_AOE_DAMAGE);
        break;
    case TARGET_DUELVSPLAYER:
    {
        Unit *target = m_targets.getUnitTarget();
        if(target)
        {
            if(m_caster->IsFriendlyTo(target))
            {
                targetUnitMap.push_back(target);
            }
            else
            {
                if(Unit* pUnitTarget = m_caster->SelectMagnetTarget(m_targets.getUnitTarget(), this, effIndex))
                {
                    m_targets.setUnitTarget(pUnitTarget);
                    targetUnitMap.push_back(pUnitTarget);
                }
            }
        }
        break;
    }
    case TARGET_GAMEOBJECT_ITEM:
        if (m_targets.getGOTargetGuid())
            AddGOTarget(m_targets.getGOTarget(), effIndex);
        else if (m_targets.getItemTarget())
            AddItemTarget(m_targets.getItemTarget(), effIndex);
        break;
    case TARGET_MASTER:
        if(Unit* owner = m_caster->GetCharmerOrOwner())
            targetUnitMap.push_back(owner);
        break;
    case TARGET_ALL_ENEMY_IN_AREA_CHANNELED:
        // targets the ground, not the units in the area
        FillAreaTargets(targetUnitMap, radius, PUSH_DEST_CENTER, SPELL_TARGETS_AOE_DAMAGE);
        break;
    case TARGET_MINION:
        if(m_spellInfo->Effect[effIndex] != SPELL_EFFECT_DUEL)
            targetUnitMap.push_back(m_caster);
        break;
    case TARGET_AREAEFFECT_PARTY:
    {
        Unit* owner = m_caster->GetCharmerOrOwner();
        Player *pTarget = NULL;

        if(owner)
        {
            targetUnitMap.push_back(m_caster);
            if(owner->GetTypeId() == TYPEID_PLAYER)
                pTarget = (Player*)owner;
        }
        else if (m_caster->GetTypeId() == TYPEID_PLAYER)
        {
            if(Unit* target = m_targets.getUnitTarget())
            {
                if( target->GetTypeId() != TYPEID_PLAYER)
                {
                    if(((Creature*)target)->IsPet())
                    {
                        Unit *targetOwner = target->GetOwner();
                        if(targetOwner->GetTypeId() == TYPEID_PLAYER)
                            pTarget = (Player*)targetOwner;
                    }
                }
                else
                    pTarget = (Player*)target;
            }
        }

        Group* pGroup = pTarget ? pTarget->GetGroup() : NULL;

        if(pGroup)
        {
            uint8 subgroup = pTarget->GetSubGroup();

            for(GroupReference *itr = pGroup->GetFirstMember(); itr != NULL; itr = itr->next())
            {
                Player* Target = itr->getSource();

                // IsHostileTo check duel and controlled by enemy
                if(Target && Target->GetSubGroup() == subgroup && !m_caster->IsHostileTo(Target))
                {
                    if( pTarget->IsWithinDistInMap(Target, radius) )
                        targetUnitMap.push_back(Target);

                    if(Pet* pet = Target->GetPet())
                        if( pTarget->IsWithinDistInMap(pet, radius) )
                            targetUnitMap.push_back(pet);
                }
            }
        }
        else if (owner)
        {
            if(m_caster->IsWithinDistInMap(owner, radius))
                targetUnitMap.push_back(owner);
        }
        else if(pTarget)
        {
            targetUnitMap.push_back(pTarget);

            if(Pet* pet = pTarget->GetPet())
                if( m_caster->IsWithinDistInMap(pet, radius) )
                    targetUnitMap.push_back(pet);
        }
        break;
    }
    case TARGET_SCRIPT:
    {
        if(m_targets.getUnitTarget())
            targetUnitMap.push_back(m_targets.getUnitTarget());
        if(m_targets.getItemTarget())
            AddItemTarget(m_targets.getItemTarget(), effIndex);
        break;
    }
    case TARGET_SELF_FISHING:
        targetUnitMap.push_back(m_caster);
        break;
    case TARGET_CHAIN_HEAL:
    {
        Unit* pUnitTarget = m_targets.getUnitTarget();
        if(!pUnitTarget)
            break;

        if (EffectChainTarget <= 1)
            targetUnitMap.push_back(pUnitTarget);
        else
        {
            unMaxTargets = EffectChainTarget;
            float max_range = radius + unMaxTargets * CHAIN_SPELL_JUMP_RADIUS;

            UnitList tempTargetUnitMap;

            FillAreaTargets(tempTargetUnitMap, max_range, PUSH_SELF_CENTER, SPELL_TARGETS_FRIENDLY);

            if (m_caster != pUnitTarget && std::find(tempTargetUnitMap.begin(), tempTargetUnitMap.end(), m_caster) == tempTargetUnitMap.end())
                tempTargetUnitMap.push_front(m_caster);

            tempTargetUnitMap.sort(TargetDistanceOrderNear(pUnitTarget));

            if (tempTargetUnitMap.empty())
                break;

            if (*tempTargetUnitMap.begin() == pUnitTarget)
                tempTargetUnitMap.erase(tempTargetUnitMap.begin());

            targetUnitMap.push_back(pUnitTarget);
            uint32 t = unMaxTargets - 1;
            Unit *prev = pUnitTarget;
            UnitList::iterator next = tempTargetUnitMap.begin();

            while(t && next != tempTargetUnitMap.end())
            {
                if(!prev->IsWithinDist(*next, CHAIN_SPELL_JUMP_RADIUS))
                    break;

                if(!prev->IsWithinLOSInMap(*next))
                {
                    ++next;
                    continue;
                }

                if((*next)->GetHealth() == (*next)->GetMaxHealth())
                {
                    next = tempTargetUnitMap.erase(next);
                    continue;
                }

                prev = *next;
                targetUnitMap.push_back(prev);
                tempTargetUnitMap.erase(next);
                tempTargetUnitMap.sort(TargetDistanceOrderNear(prev));
                next = tempTargetUnitMap.begin();

                --t;
            }
        }
        break;
    }
    case TARGET_CURRENT_ENEMY_COORDINATES:
    {
        Unit* currentTarget = m_targets.getUnitTarget();
        if(currentTarget)
        {
            targetUnitMap.push_back(currentTarget);
            m_targets.setDestination(currentTarget->GetPositionX(), currentTarget->GetPositionY(), currentTarget->GetPositionZ());
        }
        break;
    }
    case TARGET_AREAEFFECT_PARTY_AND_CLASS:
    {
        Player* targetPlayer = m_targets.getUnitTarget() && m_targets.getUnitTarget()->GetTypeId() == TYPEID_PLAYER
                               ? (Player*)m_targets.getUnitTarget() : NULL;

        Group* pGroup = targetPlayer ? targetPlayer->GetGroup() : NULL;
        if(pGroup)
        {
            for(GroupReference *itr = pGroup->GetFirstMember(); itr != NULL; itr = itr->next())
            {
                Player* Target = itr->getSource();

                // IsHostileTo check duel and controlled by enemy
                if( Target && targetPlayer->IsWithinDistInMap(Target, radius) &&
                        targetPlayer->getClass() == Target->getClass() &&
                        !m_caster->IsHostileTo(Target) )
                {
                    targetUnitMap.push_back(Target);
                }
            }
        }
        else if(m_targets.getUnitTarget())
            targetUnitMap.push_back(m_targets.getUnitTarget());
        break;
    }
    case TARGET_TABLE_X_Y_Z_COORDINATES:
    {
        SpellTargetPosition const* st = sSpellMgr.GetSpellTargetPosition(m_spellInfo->Id);
        if(st)
        {
            // teleportspells are handled in another way
            if (m_spellInfo->Effect[effIndex] == SPELL_EFFECT_TELEPORT_UNITS)
                break;
            if (st->target_mapId == m_caster->GetMapId())
                m_targets.setDestination(st->target_X, st->target_Y, st->target_Z);
            else
                sLog.outError( "SPELL: wrong map (%u instead %u) target coordinates for spell ID %u", st->target_mapId, m_caster->GetMapId(), m_spellInfo->Id );
        }
        else
            sLog.outError( "SPELL: unknown target coordinates for spell ID %u", m_spellInfo->Id );
        break;
    }
    case TARGET_DYNAMIC_OBJECT_FRONT:
    case TARGET_DYNAMIC_OBJECT_BEHIND:
    case TARGET_DYNAMIC_OBJECT_LEFT_SIDE:
    case TARGET_DYNAMIC_OBJECT_RIGHT_SIDE:
    {
        if (!(m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION))
        {
            // General override, we don't want to use max spell range here.
            // Note: 0.0 radius is also for index 36. It is possible that 36 must be defined as
            // "at the base of", in difference to 0 which appear to be "directly in front of".
            // TODO: some summoned will make caster be half inside summoned object. Need to fix
            // that in the below code (nearpoint vs closepoint, etc).
            if (m_spellInfo->EffectRadiusIndex[effIndex] == 0)
                radius = 0.0f;

            float angle = m_caster->GetOrientation();
            switch(targetMode)
            {
            case TARGET_DYNAMIC_OBJECT_FRONT:
                break;
            case TARGET_DYNAMIC_OBJECT_BEHIND:
                angle += M_PI_F;
                break;
            case TARGET_DYNAMIC_OBJECT_LEFT_SIDE:
                angle += M_PI_F / 2;
                break;
            case TARGET_DYNAMIC_OBJECT_RIGHT_SIDE:
                angle -= M_PI_F / 2;
                break;
            }

            float x, y;
            m_caster->GetNearPoint2D(x, y, radius, angle);
            m_targets.setDestination(x, y, m_caster->GetPositionZ());
        }

        targetUnitMap.push_back(m_caster);
        break;
    }
    case TARGET_EFFECT_SELECT:
    {
        // add here custom effects that need default target.
        // FOR EVERY TARGET TYPE THERE IS A DIFFERENT FILL!!
        switch(m_spellInfo->Effect[effIndex])
        {
        case SPELL_EFFECT_DUMMY:
        {
            switch(m_spellInfo->Id)
            {
            case 20577:                         // Cannibalize
            {
                WorldObject* result = FindCorpseUsing<MaNGOS::CannibalizeObjectCheck> ();

                if(result)
                {
                    switch(result->GetTypeId())
                    {
                    case TYPEID_UNIT:
                    case TYPEID_PLAYER:
                        targetUnitMap.push_back((Unit*)result);
                        break;
                    case TYPEID_CORPSE:
                        m_targets.setCorpseTarget((Corpse*)result);
                        if (Player* owner = ObjectAccessor::FindPlayer(((Corpse*)result)->GetOwnerGuid()))
                            targetUnitMap.push_back(owner);
                        break;
                    }
                }
                else
                {
                    // clear cooldown at fail
                    if(m_caster->GetTypeId() == TYPEID_PLAYER)
                        ((Player*)m_caster)->RemoveSpellCooldown(m_spellInfo->Id, true);
                    SendCastResult(SPELL_FAILED_NO_EDIBLE_CORPSES);
                    finish(false);
                }
                break;
            }
            default:
                if (m_targets.getUnitTarget())
                    targetUnitMap.push_back(m_targets.getUnitTarget());
                break;
            }
            break;
        }
        case SPELL_EFFECT_BIND:
        case SPELL_EFFECT_RESURRECT:
        case SPELL_EFFECT_PARRY:
        case SPELL_EFFECT_BLOCK:
        case SPELL_EFFECT_CREATE_ITEM:
        case SPELL_EFFECT_TRIGGER_SPELL:
        case SPELL_EFFECT_TRIGGER_MISSILE:
        case SPELL_EFFECT_LEARN_SPELL:
        case SPELL_EFFECT_SKILL_STEP:
        case SPELL_EFFECT_PROFICIENCY:
        case SPELL_EFFECT_SUMMON_POSSESSED:
        case SPELL_EFFECT_SUMMON_OBJECT_WILD:
        case SPELL_EFFECT_SELF_RESURRECT:
        case SPELL_EFFECT_REPUTATION:
        case SPELL_EFFECT_ADD_HONOR:
        case SPELL_EFFECT_SEND_TAXI:
            if (m_targets.getUnitTarget())
                targetUnitMap.push_back(m_targets.getUnitTarget());
            // Triggered spells have additional spell targets - cast them even if no explicit unit target is given (required for spell 50516 for example)
            else if (m_spellInfo->Effect[effIndex] == SPELL_EFFECT_TRIGGER_SPELL)
                targetUnitMap.push_back(m_caster);
            break;
        case SPELL_EFFECT_SUMMON_PLAYER:
            if (m_caster->GetTypeId()==TYPEID_PLAYER && ((Player*)m_caster)->GetSelectionGuid())
                if (Player* target = sObjectMgr.GetPlayer(((Player*)m_caster)->GetSelectionGuid()))
                    targetUnitMap.push_back(target);
            break;
        case SPELL_EFFECT_RESURRECT_NEW:
            if (m_targets.getUnitTarget())
                targetUnitMap.push_back(m_targets.getUnitTarget());
            if (m_targets.getCorpseTargetGuid())
            {
                if (Corpse *corpse = m_caster->GetMap()->GetCorpse(m_targets.getCorpseTargetGuid()))
                    if (Player* owner = ObjectAccessor::FindPlayer(corpse->GetOwnerGuid()))
                        targetUnitMap.push_back(owner);
            }
            break;
        case SPELL_EFFECT_SUMMON:
            /*[-ZERO]  if (m_spellInfo->EffectMiscValueB[effIndex] == SUMMON_TYPE_POSESSED ||
                  m_spellInfo->EffectMiscValueB[effIndex] == SUMMON_TYPE_POSESSED2)
              {
                  if(m_targets.getUnitTarget())
                      targetUnitMap.push_back(m_targets.getUnitTarget());
              }
              else */
            targetUnitMap.push_back(m_caster);
            break;
        case SPELL_EFFECT_SUMMON_CHANGE_ITEM:
        case SPELL_EFFECT_SUMMON_WILD:
        case SPELL_EFFECT_SUMMON_GUARDIAN:
        case SPELL_EFFECT_SUMMON_TOTEM: //The Gnomish Battle Chicken is appearently counted as a totem.
        case SPELL_EFFECT_TRANS_DOOR:
        case SPELL_EFFECT_ADD_FARSIGHT:
        case SPELL_EFFECT_STUCK:
        case SPELL_EFFECT_DESTROY_ALL_TOTEMS:
        case SPELL_EFFECT_SUMMON_DEMON:
        case SPELL_EFFECT_SKILL:
            switch(this->m_spellInfo->Id)
            {
            case 17646:
                targetUnitMap.push_back(m_caster);

                if (WorldObject *obj = this->GetAffectiveCasterObject())
                {
                    this->m_targets.setDestination(obj->GetPositionX(),obj->GetPositionY(),obj->GetPositionZ());
                }

                break;
            default:
                targetUnitMap.push_back(m_caster);
            }
            break;
        case SPELL_EFFECT_PERSISTENT_AREA_AURA:
            if(Unit* currentTarget = m_targets.getUnitTarget())
                m_targets.setDestination(currentTarget->GetPositionX(), currentTarget->GetPositionY(), currentTarget->GetPositionZ());
            break;
        case SPELL_EFFECT_LEARN_PET_SPELL:
            if (Pet* pet = m_caster->GetPet())
                targetUnitMap.push_back(pet);
            break;
        case SPELL_EFFECT_ENCHANT_ITEM:
        case SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY:
        case SPELL_EFFECT_DISENCHANT:
        case SPELL_EFFECT_FEED_PET:
            if(m_targets.getItemTarget())
                AddItemTarget(m_targets.getItemTarget(), effIndex);
            break;
        case SPELL_EFFECT_APPLY_AURA:
            switch(m_spellInfo->EffectApplyAuraName[effIndex])
            {
            case SPELL_AURA_ADD_FLAT_MODIFIER:  // some spell mods auras have 0 target modes instead expected TARGET_SELF(1) (and present for other ranks for same spell for example)
            case SPELL_AURA_ADD_PCT_MODIFIER:
                targetUnitMap.push_back(m_caster);
                break;
            default:                            // apply to target in other case
                if (m_targets.getUnitTarget())
                    targetUnitMap.push_back(m_targets.getUnitTarget());
                break;
            }
            break;
        case SPELL_EFFECT_APPLY_AREA_AURA_PARTY:
            // AreaAura
            if(m_spellInfo->Attributes == 0x9050000 || m_spellInfo->Attributes == 0x10000)
                SetTargetMap(effIndex, TARGET_AREAEFFECT_PARTY, targetUnitMap);
            break;
        case SPELL_EFFECT_SKIN_PLAYER_CORPSE:
            if (m_targets.getUnitTarget())
                targetUnitMap.push_back(m_targets.getUnitTarget());
            else if (m_targets.getCorpseTargetGuid())
            {
                if (Corpse *corpse = m_caster->GetMap()->GetCorpse(m_targets.getCorpseTargetGuid()))
                    if (Player* owner = ObjectAccessor::FindPlayer(corpse->GetOwnerGuid()))
                        targetUnitMap.push_back(owner);
            }
            break;
        default:
            break;
        }
        break;
    }
    default:
        //sLog.outError( "SPELL: Unknown implicit target (%u) for spell ID %u", targetMode, m_spellInfo->Id );
        break;
    }

    if (unMaxTargets && targetUnitMap.size() > unMaxTargets)
    {
        // make sure one unit is always removed per iteration
        uint32 removed_utarget = 0;
        for (UnitList::iterator itr = targetUnitMap.begin(), next; itr != targetUnitMap.end(); itr = next)
        {
            next = itr;
            ++next;
            if (!*itr) continue;
            if ((*itr) == m_targets.getUnitTarget())
            {
                targetUnitMap.erase(itr);
                removed_utarget = 1;
            }
        }
        // remove random units from the map
        while (targetUnitMap.size() > unMaxTargets - removed_utarget)
        {
            uint32 poz = urand(0, targetUnitMap.size()-1);
            for (UnitList::iterator itr = targetUnitMap.begin(); itr != targetUnitMap.end(); ++itr, --poz)
            {
                if (!*itr) continue;

                if (!poz)
                {
                    targetUnitMap.erase(itr);
                    break;
                }
            }
        }
        // the player's target will always be added to the map
        if (removed_utarget && m_targets.getUnitTarget())
            targetUnitMap.push_back(m_targets.getUnitTarget());
    }
}

void Spell::prepare(SpellCastTargets const* targets, Aura* triggeredByAura)
{
    m_targets = *targets;

    m_spellState = SPELL_STATE_PREPARING;

    m_castPositionX = m_caster->GetPositionX();
    m_castPositionY = m_caster->GetPositionY();
    m_castPositionZ = m_caster->GetPositionZ();
    m_castOrientation = m_caster->GetOrientation();

    if(triggeredByAura)
        m_triggeredByAuraSpell  = triggeredByAura->GetSpellProto();

    // create and add update event for this spell
    SpellEvent* Event = new SpellEvent(this);
    m_caster->m_Events.AddEvent(Event, m_caster->m_Events.CalculateTime(1));

    //Prevent casting at cast another spell (ServerSide check)
    if(!m_IsTriggeredSpell && m_caster->IsNonMeleeSpellCasted(false, true,true))
    {
        SendCastResult(SPELL_FAILED_SPELL_IN_PROGRESS);
        finish(false);
        return;
    }

    // Fill cost data
    m_powerCost = CalculatePowerCost(m_spellInfo, m_caster, this, m_CastItem);

    SpellCastResult result = CheckCast(true);
    if(result != SPELL_CAST_OK && !IsAutoRepeat())          //always cast autorepeat dummy for triggering
    {
        if(triggeredByAura)
        {
            if (!m_IsTriggeredSpell)						// An update to the casterbar is not needed for a triggered spell.
                SendChannelUpdate(0);
            triggeredByAura->GetHolder()->SetAuraDuration(0);
        }
        SendCastResult(result);
        finish(false);
        return;
    }

    // Prepare data for triggers
    prepareDataForTriggerSystem();

    // calculate cast time (calculated after first CheckCast check to prevent charge counting for first CheckCast fail)
    m_casttime = GetSpellCastTime(m_spellInfo, this);
    m_duration = CalculateSpellDuration(m_spellInfo, m_caster);

    // Charge stun delay (Charge, Intercept, Feral Charge).
    if(m_spellInfo->Id == 7922 || m_spellInfo->Id == 20615 || m_spellInfo->Id == 20614 || m_spellInfo->Id == 20253 || m_spellInfo->Id == 19675)
    {
        m_casttime = m_caster->GetDistance(targets->getUnitTarget())*15;
        ((Player*)m_caster)->SetChargeTimer(m_casttime+50);
        if(m_spellInfo->Id == 19675)
        {
            EffectInterruptCast(EFFECT_INDEX_2);
        }
    }

    // set timer base at cast time
    ReSetTimer();
        
    // Make sure the caster is added to the list of attackers when casting a spell against a hostile target.
    Unit* target = m_targets.getUnitTarget();

    if (target && m_caster->IsHostileTo(target) && std::find(target->getAttackers().begin(), target->getAttackers().end(), m_caster) == target->getAttackers().end()
            && m_caster->getVictim() == NULL && !m_caster->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PLAYER_CONTROLLED))
    {
        if((m_spellInfo->Id != 11578 && m_spellInfo->Id != 100 && m_spellInfo->Id != 6178 && m_spellInfo->Id != 20252
            && m_spellInfo->Id != 20616 && m_spellInfo->Id != 20617 && m_spellInfo->Id != 16979 && m_spellInfo->Id != 22641))
        {
            m_caster->Attack(target, false);
        }
    }

    // stealth must be removed at cast starting (at show channel bar)
    // skip triggered spell (item equip spell casting and other not explicit character casts/item uses)
    if ( !m_IsTriggeredSpell && isSpellBreakStealth(m_spellInfo) )
    {
        // Sap - don't exit Stealth yet to prevent getting in combat and making Sap impossible to cast
        // Removing Stealth depends on talent later
        // Pick Pocket - don't exit Stealth at all
        if (!(m_spellInfo->SpellFamilyName == SPELLFAMILY_ROGUE && (m_spellInfo->SpellFamilyFlags & UI64LIT(0x00000080) || m_spellInfo->SpellFamilyFlags & 2147483648)))
            m_caster->RemoveSpellsCausingAura(SPELL_AURA_MOD_STEALTH);

        m_caster->RemoveSpellsCausingAura(SPELL_AURA_MOD_INVISIBILITY);

        m_caster->RemoveSpellsCausingAura(SPELL_AURA_FEIGN_DEATH);
    }

    sMod.spellPrepare(this, m_caster);  // extra for prepare

    // Make the Ritual of Summoning effect instantly cast.
    if (m_spellInfo->Id == 7720)
        m_timer = 0;

    // add non-triggered (with cast time and without)
    if (!m_IsTriggeredSpell || IsChanneledSpell(m_spellInfo))
    {
        // add to cast type slot
        m_caster->SetCurrentCastedSpell( this );

        // will show cast bar
        SendSpellStart();

        TriggerGlobalCooldown();
    }
    // execute triggered without cast time explicitly in call point
    else if(m_timer == 0)
        cast(true);
    // else triggered with cast time will execute execute at next tick or later
    // without adding to cast type slot
    // will not show cast bar but will show effects at casting time etc
}

void Spell::cancel()
{
    if(m_spellState == SPELL_STATE_FINISHED)
        return;

    // channeled spells don't display interrupted message even if they are interrupted, possible other cases with no "Interrupted" message
    bool sendInterrupt = IsChanneledSpell(m_spellInfo) ? false : true;

    m_autoRepeat = false;
    switch (m_spellState)
    {
    case SPELL_STATE_PREPARING:
        CancelGlobalCooldown();

        //(no break)
    case SPELL_STATE_DELAYED:
    {
        SendInterrupted(0);

        if (sendInterrupt)
            SendCastResult(SPELL_FAILED_INTERRUPTED);
    }
    break;

    case SPELL_STATE_CASTING:
    {
        for(TargetList::const_iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
        {
            if (ihit->missCondition == SPELL_MISS_NONE)
            {
                Unit* unit = m_caster->GetObjectGuid() == (*ihit).targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, ihit->targetGUID);
                if (unit && unit->isAlive())
                    unit->RemoveAurasByCasterSpell(m_spellInfo->Id, m_caster->GetObjectGuid());
            }
        }

        SendChannelUpdate(0);
        SendInterrupted(0);

        if (sendInterrupt)
            SendCastResult(SPELL_FAILED_INTERRUPTED);
    }
    break;

    default:
    {
    } break;
    }

    // Remove player from attacker list if not in combat.
    if (m_caster && !m_caster->isInCombat())
        m_caster->AttackStop();

    finish(false);
    m_caster->RemoveDynObject(m_spellInfo->Id);
    m_caster->RemoveGameObject(m_spellInfo->Id, true);
}

void Spell::cast(bool skipCheck)
{
    if (m_caster->GetTypeId() == TYPEID_UNIT)
        m_caster->GetMotionMaster()->SuspendChaseMovement();


    SetExecutedCurrently(true);
    m_startedCasting = time(NULL);

    if (!m_caster->CheckAndIncreaseCastCounter())
    {
        if (m_triggeredByAuraSpell)
            sLog.outError("Spell %u triggered by aura spell %u too deep in cast chain for cast. Cast not allowed for prevent overflow stack crash.", m_spellInfo->Id, m_triggeredByAuraSpell->Id);
        else
            sLog.outError("Spell %u too deep in cast chain for cast. Cast not allowed for prevent overflow stack crash.", m_spellInfo->Id);

        SendCastResult(SPELL_FAILED_ERROR);
        finish(false);
        SetExecutedCurrently(false);
        return;
    }

    // update pointers base at GUIDs to prevent access to already nonexistent object
    UpdatePointers();

    // cancel at lost main target unit
    if (!m_targets.getUnitTarget() && m_targets.getUnitTargetGuid() && m_targets.getUnitTargetGuid() != m_caster->GetObjectGuid())
    {
        cancel();
        m_caster->DecreaseCastCounter();
        SetExecutedCurrently(false);
        return;
    }

    if (m_caster->GetTypeId() != TYPEID_PLAYER && m_targets.getUnitTarget() && m_targets.getUnitTarget() != m_caster)
        m_caster->SetInFront(m_targets.getUnitTarget());

    SpellCastResult castResult = CheckPower();
    if (castResult != SPELL_CAST_OK)
    {
        SendCastResult(castResult);
        finish(false);
        m_caster->DecreaseCastCounter();
        SetExecutedCurrently(false);
        return;
    }

    // triggered cast called from Spell::prepare where it was already checked
    if (!skipCheck)
    {
        castResult = CheckCast(false);
        if(castResult != SPELL_CAST_OK)
        {
            SendCastResult(castResult);
            finish(false);
            m_caster->DecreaseCastCounter();
            SetExecutedCurrently(false);
            return;
        }
    }

    unitTarget = m_targets.getUnitTarget();

    SpellEntry const* spellProto = sSpellStore.LookupEntry(m_spellInfo->Id);

    if (!CheckHOTStacking(spellProto))
    {
        SendCastResult(SPELL_FAILED_MORE_POWERFUL_SPELL_ACTIVE);
        cancel();
        finish(false);
        m_caster->DecreaseCastCounter();
        SetExecutedCurrently(false);
        return;
    }


    // different triggred (for caster) and precast (casted before apply effect to target) cases
    switch(m_spellInfo->SpellFamilyName)
    {
    case SPELLFAMILY_GENERIC:
    {
        // Bandages
        if (m_spellInfo->Mechanic == MECHANIC_BANDAGE)
            AddPrecastSpell(11196);                     // Recently Bandaged
        // Divine Shield, Divine Protection (Blessing of Protection in paladin switch case)
        else if(m_spellInfo->Mechanic == MECHANIC_INVULNERABILITY)
            AddPrecastSpell(25771);                     // Forbearance
        else if (m_spellInfo->Id == 20594) // Stoneform
            AddPrecastSpell(20612);
        break;
    }

    case SPELLFAMILY_ROGUE:
    {
        //exit stealth on sap when improved sap is not skilled
        if (m_spellInfo->SpellFamilyFlags & UI64LIT(0x00000080) && m_caster->GetTypeId() == TYPEID_PLAYER &&
                (!m_caster->GetAura(14076,SpellEffectIndex(0)) && !m_caster->GetAura(14094,SpellEffectIndex(0)) && !m_caster->GetAura(14095,SpellEffectIndex(0))))
            m_caster->RemoveSpellsCausingAura(SPELL_AURA_MOD_STEALTH);
    }

    case SPELLFAMILY_WARRIOR:
        break;
	case SPELLFAMILY_DRUID:
		if(m_spellInfo->SpellFamilyName == SPELLFAMILY_DRUID && m_spellInfo->Id == 22812)		// barkskin and damage reduction
            AddPrecastSpell(22839);   
		break;
    case SPELLFAMILY_PRIEST:
    {
        // Power Word: Shield
        if(m_spellInfo->SpellFamilyName == SPELLFAMILY_PRIEST && m_spellInfo->SpellFamilyFlags & UI64LIT(0x0000000000000001))
            AddPrecastSpell(6788);                      // Weakened Soul

        switch(m_spellInfo->Id)
        {
        case 15237:
            AddTriggeredSpell(23455);
            break;// Holy Nova, rank 1
        case 15430:
            AddTriggeredSpell(23458);
            break;// Holy Nova, rank 2
        case 15431:
            AddTriggeredSpell(23459);
            break;// Holy Nova, rank 3
        case 27799:
            AddTriggeredSpell(27803);
            break;// Holy Nova, rank 4
        case 27800:
            AddTriggeredSpell(27804);
            break;// Holy Nova, rank 5
        case 27801:
            AddTriggeredSpell(27805);
            break;// Holy Nova, rank 6
        case 25331:
            AddTriggeredSpell(25329);
            break;// Holy Nova, rank 7
        default:
            break;
        }
        break;
    }
    case SPELLFAMILY_PALADIN:
    {
        // Blessing of Protection (Divine Shield, Divine Protection in generic switch case)
        if(m_spellInfo->Mechanic == MECHANIC_INVULNERABILITY && m_spellInfo->Id != 25771)
            AddPrecastSpell(25771);                     // Forbearance
        break;
    }
    default:
        break;
    }

    // traded items have trade slot instead of guid in m_itemTargetGUID
    // set to real guid to be sent later to the client
    m_targets.updateTradeSlotItem();

    FillTargetMap();

    if(m_spellState == SPELL_STATE_FINISHED)                // stop cast if spell marked as finish somewhere in FillTargetMap
    {
        m_caster->DecreaseCastCounter();
        SetExecutedCurrently(false);
        return;
    }

    if (!CheckPaladinBlessingStacking(spellProto))
    {
        SendCastResult(SPELL_FAILED_MORE_POWERFUL_SPELL_ACTIVE);
        cancel();
        finish(false);
        m_caster->DecreaseCastCounter();
        SetExecutedCurrently(false);
        return;
    }

    // TODO: Filter out equipment spells from the check.
    if (!CheckBuffOverwrite(spellProto))
    {
        SendCastResult(SPELL_FAILED_MORE_POWERFUL_SPELL_ACTIVE);
        cancel();
        finish(false);
        m_caster->DecreaseCastCounter();
        SetExecutedCurrently(false);
        return;
    }

    // CAST SPELL
    SendSpellCooldown();

    TakePower();
    TakeReagents();                                         // we must remove reagents before HandleEffects to allow place crafted item in same slot

    SendCastResult(castResult);
    SendSpellGo();                                          // we must send smsg_spell_go packet before m_castItem delete in TakeCastItem()...

    InitializeDamageMultipliers();

    // Okay, everything is prepared. Now we need to distinguish between immediate and evented delayed spells
    if (m_spellInfo->speed > 0.0f)
    {

        // Remove used for cast item if need (it can be already NULL after TakeReagents call
        // in case delayed spell remove item at cast delay start
        TakeCastItem();

        // fill initial spell damage from caster for delayed casted spells
        for(TargetList::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
            HandleDelayedSpellLaunch(&(*ihit));

        // Okay, maps created, now prepare flags
        m_immediateHandled = false;
        m_spellState = SPELL_STATE_DELAYED;
        SetDelayStart(0);
    }
    else
    {
        // Immediate spell, no big deal
        handle_immediate();
    }
    
     // Remove spell mods that should only be removed if the spell was successfully cast.
    if (m_caster->GetTypeId() == TYPEID_PLAYER)
        dynamic_cast<Player*>(m_caster)->RemoveSpellModsOnSpellSuccess(this);

    // Handle auras that should have charges removed on cast.
    // This has to be done after the spell has been handled.
    HandleAuraDecayAtSpellCast();

    // Update spellmods that were affected at cast.
    if (m_caster->GetTypeId() == TYPEID_PLAYER)
        ((Player*) m_caster)->RemoveSpellMods(this);

    m_caster->DecreaseCastCounter();
    SetExecutedCurrently(false);
}

void Spell::handle_immediate()
{
    // process immediate effects (items, ground, etc.) also initialize some variables
    _handle_immediate_phase();

    bool resist = false;

    // start channeling if applicable (after _handle_immediate_phase for get persistent effect dynamic object for channel target
    if (IsChanneledSpell(m_spellInfo) && m_duration)
    {
        if (!m_UniqueTargetInfo.empty() &&
                //Drain Life
                ((m_spellInfo->SpellIconID == 546 && m_spellInfo->SpellVisual == 177)
                 //Drain Soul
                 || (m_spellInfo->SpellIconID == 113 && m_spellInfo->SpellVisual == 788)
                 //Drain Mana + Mind Flay
                 || (m_spellInfo->SpellIconID == 548 && m_spellInfo->SpellVisual == 277)
                 //Mind Control
                 || (m_spellInfo->SpellIconID == 235 && m_spellInfo->SpellVisual == 137)
                 //Mind Vision
                 || (m_spellInfo->SpellIconID == 502 && m_spellInfo->SpellVisual == 4839)))
        {
            SpellMissInfo missInfo = m_UniqueTargetInfo.begin()->missCondition;
            if (missInfo == SPELL_MISS_MISS || missInfo == SPELL_MISS_RESIST)
                resist = true;
        }
        m_spellState = SPELL_STATE_CASTING;
        SendChannelStart(m_duration);
    }

    for(TargetList::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
        DoAllEffectOnTarget(&(*ihit));

    for(GOTargetList::iterator ihit = m_UniqueGOTargetInfo.begin(); ihit != m_UniqueGOTargetInfo.end(); ++ihit)
        DoAllEffectOnTarget(&(*ihit));

    // spell is finished, perform some last features of the spell here
    _handle_finish_phase();

    // Remove used for cast item if need (it can be already NULL after TakeReagents call
    TakeCastItem();

    if(m_spellState != SPELL_STATE_CASTING)
        finish(true);                                       // successfully finish spell cast (not last in case autorepeat or channel spell)

    if(resist && m_caster)
        m_caster->InterruptSpell(CURRENT_CHANNELED_SPELL);

}

uint64 Spell::handle_delayed(uint64 t_offset)
{
    uint64 next_time = 0;

    if (!m_immediateHandled)
    {
        _handle_immediate_phase();
        m_immediateHandled = true;
    }

    // now recheck units targeting correctness (need before any effects apply to prevent adding immunity at first effect not allow apply second spell effect and similar cases)
    for(TargetList::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        // Recheck immunities on spell hit.
        if (m_caster)
        {
            Unit* unit = m_caster->GetObjectGuid() == ihit->targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, ihit->targetGUID);
            if (unit && unit->IsImmuneToSpell(m_spellInfo))
            {
                ihit->processed = true;
                continue;
            }
        }

        if (!ihit->processed)
        {
            if (ihit->timeDelay <= t_offset)
                DoAllEffectOnTarget(&(*ihit));
            else if (next_time == 0 || ihit->timeDelay < next_time)
                next_time = ihit->timeDelay;
        }
    }

    // now recheck gameobject targeting correctness
    for(GOTargetList::iterator ighit = m_UniqueGOTargetInfo.begin(); ighit != m_UniqueGOTargetInfo.end(); ++ighit)
    {
        if (!ighit->processed)
        {
            if (ighit->timeDelay <= t_offset)
                DoAllEffectOnTarget(&(*ighit));
            else if (next_time == 0 || ighit->timeDelay < next_time)
                next_time = ighit->timeDelay;
        }
    }
    // All targets passed - need finish phase
    if (next_time == 0)
    {
        // spell is finished, perform some last features of the spell here
        _handle_finish_phase();

        finish(true);                                       // successfully finish spell cast

        // return zero, spell is finished now
        return 0;
    }
    else
    {
        // spell is unfinished, return next execution time
        return next_time;
    }
}

void Spell::HandleAuraDecayAtSpellCast()
{
    if (m_spellInfo->DmgClass)
    {
        // Unstable Power
        if (m_caster->HasAura(24659) && (m_damage > 0 || m_healing > 0 || 
            m_spellInfo->EffectApplyAuraName[0] == SPELL_AURA_PERIODIC_DAMAGE || 
            m_spellInfo->EffectApplyAuraName[0] == SPELL_AURA_PERIODIC_HEAL ||
            m_spellInfo->EffectApplyAuraName[0] == SPELL_AURA_PERIODIC_LEECH))
        {
            // Need remove one 24659 aura
            m_caster->RemoveAuraHolderFromStack(24659);
        }
        
        // Aura 24389 is the buff received from the trinket Fire Ruby (Mage quest reward).
        if(m_caster->HasAura(24389) && ((strcmp("Blast Wave", *m_spellInfo->SpellName) == 0) || (strcmp("Flamestrike", *m_spellInfo->SpellName) == 0)))
        {
            m_caster->RemoveAurasDueToSpell(24389); // Consume aura, remove it.
        }
    }
}

void Spell::_handle_immediate_phase()
{
    // handle some immediate features of the spell here
    HandleThreatSpells();

    m_needSpellLog = IsNeedSendToClient();
    for(int j = 0; j < MAX_EFFECT_INDEX; ++j)
    {
        if(m_spellInfo->Effect[j] == 0)
            continue;

        // apply Send Event effect to ground in case empty target lists
        if( m_spellInfo->Effect[j] == SPELL_EFFECT_SEND_EVENT && !HaveTargetsForEffect(SpellEffectIndex(j)) )
        {
            HandleEffects(NULL, NULL, NULL, SpellEffectIndex(j));
            continue;
        }

        // Don't do spell log, if is school damage spell
        if(m_spellInfo->Effect[j] == SPELL_EFFECT_SCHOOL_DAMAGE || m_spellInfo->Effect[j] == 0)
            m_needSpellLog = false;
    }

    // initialize Diminishing Returns Data
    m_diminishLevel = DIMINISHING_LEVEL_1;
    m_diminishGroup = DIMINISHING_NONE;

    // process items
    for(ItemTargetList::iterator ihit = m_UniqueItemInfo.begin(); ihit != m_UniqueItemInfo.end(); ++ihit)
        DoAllEffectOnTarget(&(*ihit));

    // process ground
    for(int j = 0; j < MAX_EFFECT_INDEX; ++j)
    {
        // persistent area auras target only the ground
        if(m_spellInfo->Effect[j] == SPELL_EFFECT_PERSISTENT_AREA_AURA)
            HandleEffects(NULL, NULL, NULL, SpellEffectIndex(j));
    }
}

void Spell::_handle_finish_phase()
{
    // spell log
    if(m_needSpellLog)
        SendLogExecute();
}

void Spell::SendSpellCooldown()
{
    if(m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    Player* _player = (Player*)m_caster;

    // (1) have infinity cooldown but set at aura apply, (2) passive cooldown at triggering
    if(m_spellInfo->Attributes & (SPELL_ATTR_DISABLED_WHILE_ACTIVE | SPELL_ATTR_PASSIVE))
        return;

    _player->AddSpellAndCategoryCooldowns(m_spellInfo, m_CastItem ? m_CastItem->GetEntry() : 0, this);
}

void Spell::update(uint32 difftime)
{
    // update pointers based at it's GUIDs
    UpdatePointers();

    if (m_targets.getUnitTargetGuid() && !m_targets.getUnitTarget())
    {
        cancel();
        return;
    }

    // check if the player caster has moved before the spell finished
    if ((m_caster->GetTypeId() == TYPEID_PLAYER && m_timer != 0) &&
            (m_castPositionX != m_caster->GetPositionX() || m_castPositionY != m_caster->GetPositionY() || m_castPositionZ != m_caster->GetPositionZ()) &&
            (m_spellInfo->Effect[EFFECT_INDEX_0] != SPELL_EFFECT_STUCK || !((Player*)m_caster)->m_movementInfo.HasMovementFlag(MOVEFLAG_FALLING_FAR)))
    {
        // always cancel for channeled spells
        if( m_spellState == SPELL_STATE_CASTING )
            cancel();
        // don't cancel for melee, autorepeat, triggered and instant spells
        else if(!IsNextMeleeSwingSpell() && !IsAutoRepeat() && !m_IsTriggeredSpell && (m_spellInfo->InterruptFlags & SPELL_INTERRUPT_FLAG_MOVEMENT))
            cancel();
    }

    switch(m_spellState)
    {
    case SPELL_STATE_PREPARING:
    {
        if(m_timer)
        {
            if(difftime >= m_timer)
                m_timer = 0;
            else
                m_timer -= difftime;
        }

        //Check cast for non-player units
        if( m_caster->GetTypeId() == TYPEID_UNIT && m_timer != 0 && !IsNextMeleeSwingSpell() && !IsAutoRepeat()
                && (m_caster->hasUnitState(UNIT_STAT_CAN_NOT_REACT_OR_LOST_CONTROL) || m_caster->hasUnitState(UNIT_STAT_FLEEING_MOVE)))
            cancel();

        if(m_timer == 0 && !IsNextMeleeSwingSpell() && !IsAutoRepeat())
            cast();
    }
    break;
    case SPELL_STATE_CASTING:
    {
        if(m_timer > 0)
        {
            if( m_caster->GetTypeId() == TYPEID_PLAYER )
            {
                // check if player has jumped before the channeling finished
                if(((Player*)m_caster)->m_movementInfo.HasMovementFlag(MOVEFLAG_FALLING))
                    cancel();

                // check for incapacitating player states
                if( m_caster->hasUnitState(UNIT_STAT_CAN_NOT_REACT))
                    cancel();

                // check if player has turned if flag is set
                if( m_spellInfo->ChannelInterruptFlags & CHANNEL_FLAG_TURNING && m_castOrientation != m_caster->GetOrientation() )
                    cancel();
            }

            // check if there are alive targets left
            if (!IsAliveUnitPresentInTargetList())
            {
                SendChannelUpdate(0);
                finish();
            }

            if(difftime >= m_timer)
                m_timer = 0;
            else
                m_timer -= difftime;
        }

        if(m_timer == 0)
        {
            SendChannelUpdate(0);

            // channeled spell processed independently for quest targeting
            // cast at creature (or GO) quest objectives update at successful cast channel finished
            // ignore autorepeat/melee casts for speed (not exist quest for spells (hm... )
            if( !IsAutoRepeat() && !IsNextMeleeSwingSpell() )
            {
                if ( Player* p = m_caster->GetCharmerOrOwnerPlayerOrPlayerItself() )
                {
                    for(TargetList::const_iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
                    {
                        TargetInfo const& target = *ihit;
                        if (!target.targetGUID.IsCreature())
                            continue;

                        Unit* unit = m_caster->GetObjectGuid() == target.targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, target.targetGUID);
                        if (unit == NULL)
                            continue;

                        p->RewardPlayerAndGroupAtCast(unit, m_spellInfo->Id);
                    }

                    for(GOTargetList::const_iterator ihit = m_UniqueGOTargetInfo.begin(); ihit != m_UniqueGOTargetInfo.end(); ++ihit)
                    {
                        GOTargetInfo const& target = *ihit;

                        GameObject* go = m_caster->GetMap()->GetGameObject(target.targetGUID);
                        if(!go)
                            continue;

                        p->RewardPlayerAndGroupAtCast(go, m_spellInfo->Id);
                    }
                }
            }

            finish();
        }
    }
    break;
    default:
    {
    } break;
    }
}

void Spell::finish(bool ok)
{
    if(!m_caster)
        return;

    if (m_caster->GetTypeId() == TYPEID_UNIT)
        m_caster->GetMotionMaster()->ResumeChaseMovement();

    if(m_spellState == SPELL_STATE_FINISHED)
        return;

    m_spellState = SPELL_STATE_FINISHED;

    if (m_spellInfo->Id == 23017 && !ok)
    {
        Player* pPlayer = dynamic_cast<Player*>(m_caster);

        if (pPlayer)
        {
            Player* owner = pPlayer->GetMap()->GetPlayer(pPlayer->m_summonMasterGuid);
            if (owner)
                owner->InterruptSpell(CURRENT_CHANNELED_SPELL);
            
            Player* participant = pPlayer->GetMap()->GetPlayer(pPlayer->m_summonParticipantGuid);
            if (participant)
                participant->InterruptSpell(CURRENT_CHANNELED_SPELL);
        }

    }

    // other code related only to successfully finished spells
    if(!ok)
        return;

    // remove spell mods
    if (m_caster->GetTypeId() == TYPEID_PLAYER)
    {
        ((Player*)m_caster)->RemoveSpellMods(this);
    }

    // handle SPELL_AURA_ADD_TARGET_TRIGGER auras
    if (!IsChanneledSpell(m_spellInfo))
    {
        Unit::AuraList const& targetTriggers = m_caster->GetAurasByType(SPELL_AURA_ADD_TARGET_TRIGGER);
        for(Unit::AuraList::const_iterator i = targetTriggers.begin(); i != targetTriggers.end(); ++i)
        {
            if (!(*i)->isAffectedOnSpell(m_spellInfo))
                continue;
            for(TargetList::const_iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
            {
                if (ihit->missCondition == SPELL_MISS_NONE)
                {
                    // check m_caster->GetGUID() let load auras at login and speedup most often case
                    Unit *unit = m_caster->GetObjectGuid() == ihit->targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, ihit->targetGUID);
                    if (unit && unit->isAlive())
                    {
                        SpellEntry const *auraSpellInfo = (*i)->GetSpellProto();
                        SpellEffectIndex auraSpellIdx = (*i)->GetEffIndex();
                        // Calculate chance at that moment (can be depend for example from combo points)
                        int32 auraBasePoints = (*i)->GetBasePoints();
                        int32 chance = m_caster->CalculateSpellDamage(unit, auraSpellInfo, auraSpellIdx, &auraBasePoints);
                        if(roll_chance_i(chance))
                            m_caster->CastSpell(unit, auraSpellInfo->EffectTriggerSpell[auraSpellIdx], true, NULL, (*i));
                    }
                }
            }
        }
    }

    // Heal caster for all health leech from all targets
    if (m_healthLeech)
        m_caster->DealHeal(m_caster, uint32(m_healthLeech), m_spellInfo);

    if (IsMeleeAttackResetSpell())
    {
        m_caster->resetAttackTimer(BASE_ATTACK);
        if(m_caster->haveOffhandWeapon())
            m_caster->resetAttackTimer(OFF_ATTACK);
    }

    /*if (IsRangedAttackResetSpell())
        m_caster->resetAttackTimer(RANGED_ATTACK);*/

    // Clear combo at finish state
    if(m_caster->GetTypeId() == TYPEID_PLAYER && NeedsComboPoints(m_spellInfo))
    {
        // Not drop combopoints if negative spell and if any miss on enemy exist
        bool needDrop = true;
        if (!IsPositiveSpell(m_spellInfo->Id))
        {
            for(TargetList::const_iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
            {
                if (ihit->missCondition != SPELL_MISS_NONE && ihit->targetGUID != m_caster->GetObjectGuid())
                {
                    needDrop = false;
                    break;
                }
            }
        }
        if (needDrop)
            ((Player*)m_caster)->ClearComboPoints();
    }
    
    // If the spell is triggered by a warrior's execute it consumes all rage.
    if (m_spellInfo->Id == 20647)
    {
        m_caster->SetPower(POWER_RAGE, 0);
    }

    // call triggered spell only at successful cast (after clear combo points -> for add some if need)
    if(!m_TriggerSpells.empty())
        CastTriggerSpells();

    // cast active spell on totem summon
    if (m_caster->GetTypeId() == TYPEID_UNIT && (Totem*)m_caster)
    {
        uint32 spellId = 0;
        switch (m_spellInfo->Id)
        {
        case 8515:
            spellId = 8514;
            break; //windfury totem rank 1
        case 10609:
            spellId = 10607;
            break; //windfury totem rank 2
        case 10612:
            spellId = 10611;
            break; //windfury totem rank 3
        case 8229:
            spellId = 8230;
            break; //flametongue totem rank 1
        case 8251:
            spellId = 8250;
            break; //flametongue totem rank 2
        case 10524:
            spellId = 10521;
            break; //flametongue totem rank 3
        case 16388:
            spellId = 15036;
            break; //flametongue totem rank 4
        case 6474:
            spellId = 3600;
            break; //earthbind totem
        case 8145:
            spellId = 8146;
            break; //tremor totem
        case 8179:
            spellId = 8178;
            break; //grounding totem
        case 8167:
            spellId = 8168;
            break; //poison cleansing totem
        case 8172:
            spellId = 8171;
            break; //disease cleansing totem
        }

        if (spellId)
            m_caster->CastSpell(m_caster,spellId, true);
    }

    // Stop Attack for some spells
    if( m_spellInfo->Attributes & SPELL_ATTR_STOP_ATTACK_TARGET)
        m_caster->AttackStop();
}

void Spell::SendCastResult(SpellCastResult result)
{
    if (m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    if(((Player*)m_caster)->GetSession()->PlayerLoading())  // don't send cast results at loading time
        return;

    SendCastResult((Player*)m_caster, m_spellInfo, result);
}

void Spell::SendCastResult(Player* caster, SpellEntry const* spellInfo, SpellCastResult result)
{
    WorldPacket data(SMSG_CAST_FAILED, (4+1+1));
    data << uint32(spellInfo->Id);

    if(result != SPELL_CAST_OK)
    {
        data << uint8(2); // status = fail
        data << uint8(result);                                  // problem
        switch (result)
        {
        case SPELL_FAILED_REQUIRES_SPELL_FOCUS:
            data << uint32(spellInfo->RequiresSpellFocus);
            break;
        case SPELL_FAILED_REQUIRES_AREA:
            /* [-ZERO]    // hardcode areas limitation case     switch(spellInfo->Id)
                {
                    default:                                    // default case             data << uint32(spellInfo->AreaId);
                        break;
                } */
            break;
        case SPELL_FAILED_EQUIPPED_ITEM_CLASS:
            data << uint32(spellInfo->EquippedItemClass);
            data << uint32(spellInfo->EquippedItemSubClassMask);
            data << uint32(spellInfo->EquippedItemInventoryTypeMask);
            break;
        default:
            break;
        }
    }
    else
        data << uint8(0);

    caster->GetSession()->SendPacket(&data);
}

void Spell::SendSpellStart()
{
    if (!IsNeedSendToClient())
        return;

    DEBUG_FILTER_LOG(LOG_FILTER_SPELL_CAST, "Sending SMSG_SPELL_START id=%u", m_spellInfo->Id);

    uint32 castFlags = CAST_FLAG_UNKNOWN2;
    if (IsRangedSpell())
        castFlags |= CAST_FLAG_AMMO;

    WorldPacket data(SMSG_SPELL_START, (8+8+4+2+4));
    if (m_CastItem)
        data << m_CastItem->GetPackGUID();
    else
        data << m_caster->GetPackGUID();

    data << m_caster->GetPackGUID();
    data << uint32(m_spellInfo->Id);                        // spellId
    data << uint16(castFlags);                              // cast flags
    data << uint32(m_timer);                                // delay?

    data << m_targets;

    if (castFlags & CAST_FLAG_AMMO)                         // projectile info
        WriteAmmoToPacket(&data);

    m_caster->SendMessageToSet(&data, true);
}

void Spell::SendSpellGo()
{
    // not send invisible spell casting
    if (!IsNeedSendToClient() || IsExceptedFromClientUpdate())
        return;

    DEBUG_FILTER_LOG(LOG_FILTER_SPELL_CAST, "Sending SMSG_SPELL_GO id=%u", m_spellInfo->Id);

    uint32 castFlags = CAST_FLAG_UNKNOWN9;
    if (IsRangedSpell())
        castFlags |= CAST_FLAG_AMMO;                        // arrows/bullets visual

    WorldPacket data(SMSG_SPELL_GO, 53);                    // guess size

    if (m_CastItem)
        data << m_CastItem->GetPackGUID();
    else
        data << m_caster->GetPackGUID();

    data << m_caster->GetPackGUID();
    data << uint32(m_spellInfo->Id);                        // spellId
    data << uint16(castFlags);                              // cast flags

    WriteSpellGoTargets(&data);

    data << m_targets;

    if (castFlags & CAST_FLAG_AMMO)                         // projectile info
        WriteAmmoToPacket(&data);

    m_caster->SendMessageToSet(&data, true);

    //Custom animations
    if (m_spellInfo->Id == 10444)						    // Flametongue Weapon
        SendPlaySpellVisual(399);
}

void Spell::WriteAmmoToPacket(WorldPacket* data)
{
    uint32 ammoInventoryType = 0;
    uint32 ammoDisplayID = 0;

    if (m_caster->GetTypeId() == TYPEID_PLAYER)
    {
        Item *pItem = ((Player*)m_caster)->GetWeaponForAttack( RANGED_ATTACK );
        if(pItem)
        {
            ammoInventoryType = pItem->GetProto()->InventoryType;
            if( ammoInventoryType == INVTYPE_THROWN )
                ammoDisplayID = pItem->GetProto()->DisplayInfoID;
            else
            {
                uint32 ammoID = ((Player*)m_caster)->GetUInt32Value(PLAYER_AMMO_ID);
                if(ammoID)
                {
                    ItemPrototype const *pProto = ObjectMgr::GetItemPrototype( ammoID );
                    if(pProto)
                    {
                        ammoDisplayID = pProto->DisplayInfoID;
                        ammoInventoryType = pProto->InventoryType;
                    }
                }
            }
        }
    }
    else
    {
        for (uint8 i = 0; i < MAX_VIRTUAL_ITEM_SLOT; ++i)
        {
            // see Creature::SetVirtualItem for structure data
            if(uint32 item_class = m_caster->GetByteValue(UNIT_VIRTUAL_ITEM_INFO + (i * 2) + 0, VIRTUAL_ITEM_INFO_0_OFFSET_CLASS))
            {
                if (item_class == ITEM_CLASS_WEAPON)
                {
                    switch(m_caster->GetByteValue(UNIT_VIRTUAL_ITEM_INFO + (i * 2) + 0, VIRTUAL_ITEM_INFO_0_OFFSET_SUBCLASS))
                    {
                    case ITEM_SUBCLASS_WEAPON_THROWN:
                        ammoDisplayID = m_caster->GetUInt32Value(UNIT_VIRTUAL_ITEM_SLOT_DISPLAY + i);
                        ammoInventoryType = m_caster->GetByteValue(UNIT_VIRTUAL_ITEM_INFO + (i * 2) + 0, VIRTUAL_ITEM_INFO_0_OFFSET_INVENTORYTYPE);
                        break;
                    case ITEM_SUBCLASS_WEAPON_BOW:
                    case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                        ammoDisplayID = 5996;           // is this need fixing?
                        ammoInventoryType = INVTYPE_AMMO;
                        break;
                    case ITEM_SUBCLASS_WEAPON_GUN:
                        ammoDisplayID = 5998;           // is this need fixing?
                        ammoInventoryType = INVTYPE_AMMO;
                        break;
                    }

                    if (ammoDisplayID)
                        break;
                }
            }
        }
    }

    *data << uint32(ammoDisplayID);
    *data << uint32(ammoInventoryType);
}

void Spell::WriteSpellGoTargets(WorldPacket* data)
{
    // This function also fill data for channeled spells:
    // m_needAliveTargetMask req for stop channeling if one target die
    // Always hits on GO and expected all targets for Units
    *data << (uint8)(m_UniqueTargetInfo.size() + m_UniqueGOTargetInfo.size());

    for(TargetList::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        *data << ihit->targetGUID;                          // in 1.12.1 expected all targets

        if (ihit->effectMask == 0)                          // No effect apply - all immuned add state
        {
            // possibly SPELL_MISS_IMMUNE2 for this??
            ihit->missCondition = SPELL_MISS_IMMUNE2;
        }
        else if (ihit->missCondition == SPELL_MISS_NONE)    // Add only hits
            m_needAliveTargetMask |= ihit->effectMask;
    }

    for(GOTargetList::const_iterator ighit = m_UniqueGOTargetInfo.begin(); ighit != m_UniqueGOTargetInfo.end(); ++ighit)
        *data << ighit->targetGUID;                         // Always hits

    *data << uint8(0);                                      // unknown, not miss

    // Reset m_needAliveTargetMask for non channeled spell
    if(!IsChanneledSpell(m_spellInfo))
        m_needAliveTargetMask = 0;
}

void Spell::SendLogExecute()
{
    Unit *target = m_targets.getUnitTarget() ? m_targets.getUnitTarget() : m_caster;

    WorldPacket data(SMSG_SPELLLOGEXECUTE, (8+4+4+4+4+8));

    if(m_caster->GetTypeId() == TYPEID_PLAYER)
        data << m_caster->GetPackGUID();
    else
        data << target->GetPackGUID();

    data << uint32(m_spellInfo->Id);
    uint32 count1 = 1;
    data << uint32(count1);                                 // count1 (effect count?)
    for(uint32 i = 0; i < count1; ++i)
    {
        data << uint32(m_spellInfo->Effect[EFFECT_INDEX_0]);// spell effect
        uint32 count2 = 1;
        data << uint32(count2);                             // count2 (target count?)
        for(uint32 j = 0; j < count2; ++j)
        {
            switch(m_spellInfo->Effect[EFFECT_INDEX_0])
            {
            case SPELL_EFFECT_POWER_DRAIN:
                if(Unit *unit = m_targets.getUnitTarget())
                    data << unit->GetPackGUID();
                else
                    data << uint8(0);
                data << uint32(0);
                data << uint32(0);
                data << float(0);
                break;
            case SPELL_EFFECT_ADD_EXTRA_ATTACKS:
                if(Unit *unit = m_targets.getUnitTarget())
                    data << unit->GetPackGUID();
                else
                    data << uint8(0);
                data << uint32(0);                      // count?
                break;
            case SPELL_EFFECT_INTERRUPT_CAST:
                if(Unit *unit = m_targets.getUnitTarget())
                    data << unit->GetPackGUID();
                else
                    data << uint8(0);
                data << uint32(0);                      // spellid
                break;
            case SPELL_EFFECT_DURABILITY_DAMAGE:
                if(Unit *unit = m_targets.getUnitTarget())
                    data << unit->GetPackGUID();
                else
                    data << uint8(0);
                data << uint32(0);
                data << uint32(0);
                break;
            case SPELL_EFFECT_OPEN_LOCK:
            case SPELL_EFFECT_OPEN_LOCK_ITEM:
                if(Item *item = m_targets.getItemTarget())
                    data << item->GetPackGUID();
                else
                    data << uint8(0);
                break;
            case SPELL_EFFECT_CREATE_ITEM:
                data << uint32(m_spellInfo->EffectItemType[EFFECT_INDEX_0]);
                break;
            case SPELL_EFFECT_SUMMON:
            case SPELL_EFFECT_SUMMON_WILD:
            case SPELL_EFFECT_SUMMON_GUARDIAN:
            case SPELL_EFFECT_TRANS_DOOR:
            case SPELL_EFFECT_SUMMON_PET:
            case SPELL_EFFECT_SUMMON_POSSESSED:
            case SPELL_EFFECT_SUMMON_TOTEM:
            case SPELL_EFFECT_SUMMON_OBJECT_WILD:
            case SPELL_EFFECT_CREATE_HOUSE:
            case SPELL_EFFECT_DUEL:
            case SPELL_EFFECT_SUMMON_TOTEM_SLOT1:
            case SPELL_EFFECT_SUMMON_TOTEM_SLOT2:
            case SPELL_EFFECT_SUMMON_TOTEM_SLOT3:
            case SPELL_EFFECT_SUMMON_TOTEM_SLOT4:
            case SPELL_EFFECT_SUMMON_PHANTASM:
            case SPELL_EFFECT_SUMMON_CRITTER:
            case SPELL_EFFECT_SUMMON_OBJECT_SLOT1:
            case SPELL_EFFECT_SUMMON_OBJECT_SLOT2:
            case SPELL_EFFECT_SUMMON_OBJECT_SLOT3:
            case SPELL_EFFECT_SUMMON_OBJECT_SLOT4:
            case SPELL_EFFECT_SUMMON_DEMON:
                if (Unit *unit = m_targets.getUnitTarget())
                    data << unit->GetPackGUID();
                else if (m_targets.getItemTargetGuid())
                    data << m_targets.getItemTargetGuid().WriteAsPacked();
                else if (GameObject *go = m_targets.getGOTarget())
                    data << go->GetPackGUID();
                else
                    data << uint8(0);                   // guid
                break;
            case SPELL_EFFECT_FEED_PET:
                data << uint32(m_targets.getItemTargetEntry());
                break;
            case SPELL_EFFECT_DISMISS_PET:
                if(Unit *unit = m_targets.getUnitTarget())
                    data << unit->GetPackGUID();
                else
                    data << uint8(0);
                break;
            default:
                return;
            }
        }
    }

    m_caster->SendMessageToSet(&data, true);
}

void Spell::SendInterrupted(uint8 result)
{
    if (m_caster->GetTypeId() == TYPEID_PLAYER)
    {
        Player *casterPlayer = (Player*)m_caster;
        WorldPacket data(SMSG_SPELL_FAILURE, (8+4+1));
        data << m_caster->GetObjectGuid();
        data << m_spellInfo->Id;
        data << result;
        casterPlayer->GetSession()->SendPacket(&data);
    }

    WorldPacket sendAllFailure(SMSG_SPELL_FAILED_OTHER, (8+4));
    sendAllFailure << m_caster->GetObjectGuid();
    sendAllFailure << m_spellInfo->Id;
    m_caster->SendMessageToSet(&sendAllFailure, false);
}

void Spell::SendChannelUpdate(uint32 time)
{
    if(time == 0)
    {
        m_caster->RemoveAurasByCasterSpell(m_spellInfo->Id, m_caster->GetObjectGuid());

        ObjectGuid target_guid = m_caster->GetChannelObjectGuid();
        if (target_guid != m_caster->GetObjectGuid() && target_guid.IsUnit())
            if (Unit* target = ObjectAccessor::GetUnit(*m_caster, target_guid))
                target->RemoveAurasByCasterSpell(m_spellInfo->Id, m_caster->GetObjectGuid());

        m_caster->SetChannelObjectGuid(ObjectGuid());
        m_caster->SetUInt32Value(UNIT_CHANNEL_SPELL, 0);
    }

    if (m_caster->GetTypeId() == TYPEID_PLAYER)
    {
        WorldPacket data( MSG_CHANNEL_UPDATE, 4 );
        data << uint32(time);
        ((Player*)m_caster)->SendDirectMessage(&data);
    }

    //handle SPELL_AURA_ADD_TARGET_TRIGGER auras
    Unit::AuraList const& targetTriggers = m_caster->GetAurasByType(SPELL_AURA_ADD_TARGET_TRIGGER);
    for(Unit::AuraList::const_iterator i = targetTriggers.begin(); i != targetTriggers.end(); ++i)
    {
        if (!(*i)->isAffectedOnSpell(m_spellInfo))
            continue;
        for(TargetList::const_iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
        {
            if (ihit->missCondition == SPELL_MISS_NONE)
            {
                // check m_caster->GetGUID() let load auras at login and speedup most often case
                Unit *unit = m_caster->GetObjectGuid() == ihit->targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, ihit->targetGUID);
                if (unit && unit->isAlive())
                {
                    SpellEntry const *auraSpellInfo = (*i)->GetSpellProto();
                    SpellEffectIndex auraSpellIdx = (*i)->GetEffIndex();
                    // Calculate chance at that moment (can be depend for example from combo points)
                    int32 auraBasePoints = (*i)->GetBasePoints();
                    int32 chance = m_caster->CalculateSpellDamage(unit, auraSpellInfo, auraSpellIdx, &auraBasePoints);
                    if(roll_chance_i(chance))
                        m_caster->CastSpell(unit, auraSpellInfo->EffectTriggerSpell[auraSpellIdx], true, NULL, (*i));
                }
            }
        }
    }
}

void Spell::SendChannelStart(uint32 duration)
{
    WorldObject* target = NULL;

    // select dynobject created by first effect if any
    if (m_spellInfo->Effect[EFFECT_INDEX_0] == SPELL_EFFECT_PERSISTENT_AREA_AURA)
        target = m_caster->GetDynObject(m_spellInfo->Id, EFFECT_INDEX_0);
    // select first not resisted target from target list for _0_ effect
    else if (!m_UniqueTargetInfo.empty())
    {
        for(TargetList::const_iterator itr = m_UniqueTargetInfo.begin(); itr != m_UniqueTargetInfo.end(); ++itr)
        {
            if ((itr->effectMask & (1 << EFFECT_INDEX_0)) && itr->reflectResult == SPELL_MISS_NONE &&
                    itr->targetGUID != m_caster->GetObjectGuid())
            {
                target = ObjectAccessor::GetUnit(*m_caster, itr->targetGUID);
                break;
            }
        }
    }
    else if(!m_UniqueGOTargetInfo.empty())
    {
        for(GOTargetList::const_iterator itr = m_UniqueGOTargetInfo.begin(); itr != m_UniqueGOTargetInfo.end(); ++itr)
        {
            if (itr->effectMask & (1 << EFFECT_INDEX_0))
            {
                target = m_caster->GetMap()->GetGameObject(itr->targetGUID);
                break;
            }
        }
    }

    if (m_caster->GetTypeId() == TYPEID_PLAYER)
    {
        WorldPacket data( MSG_CHANNEL_START, (4+4) );
        data << uint32(m_spellInfo->Id);
        data << uint32(duration);
        ((Player*)m_caster)->SendDirectMessage(&data);
    }

    m_timer = duration;

    if (target)
        m_caster->SetChannelObjectGuid(target->GetObjectGuid());

    m_caster->SetUInt32Value(UNIT_CHANNEL_SPELL, m_spellInfo->Id);
    //handle SPELL_AURA_ADD_TARGET_TRIGGER auras
    Unit::AuraList const& targetTriggers = m_caster->GetAurasByType(SPELL_AURA_ADD_TARGET_TRIGGER);
    for(Unit::AuraList::const_iterator i = targetTriggers.begin(); i != targetTriggers.end(); ++i)
    {
        if (!(*i)->isAffectedOnSpell(m_spellInfo))
            continue;
        for(TargetList::const_iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
        {
            if (ihit->missCondition == SPELL_MISS_NONE)
            {
                // check m_caster->GetGUID() let load auras at login and speedup most often case
                Unit *unit = m_caster->GetObjectGuid() == ihit->targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, ihit->targetGUID);
                if (unit && unit->isAlive())
                {
                    SpellEntry const *auraSpellInfo = (*i)->GetSpellProto();
                    SpellEffectIndex auraSpellIdx = (*i)->GetEffIndex();
                    // Calculate chance at that moment (can be depend for example from combo points)
                    int32 auraBasePoints = (*i)->GetBasePoints();
                    int32 chance = m_caster->CalculateSpellDamage(unit, auraSpellInfo, auraSpellIdx, &auraBasePoints);
                    if(roll_chance_i(chance))
                        m_caster->CastSpell(unit, auraSpellInfo->EffectTriggerSpell[auraSpellIdx], true, NULL, (*i));
                }
            }
        }
    }
}

void Spell::SendResurrectRequest(Player* target)
{
    // Both players and NPCs can resurrect using spells - have a look at creature 28487 for example
    // However, the packet structure differs slightly

    const char* sentName = m_caster->GetTypeId() == TYPEID_PLAYER ? "" : m_caster->GetNameForLocaleIdx(target->GetSession()->GetSessionDbLocaleIndex());

    WorldPacket data(SMSG_RESURRECT_REQUEST, (8+4+strlen(sentName)+1+1+1));
    data << m_caster->GetObjectGuid();
    data << uint32(strlen(sentName) + 1);

    data << sentName;
    data << uint8(0);

    data << uint8(m_caster->GetTypeId() == TYPEID_PLAYER ? 0 : 1);
    target->GetSession()->SendPacket(&data);
}

void Spell::SendPlaySpellVisual(uint32 SpellID)
{
    if (m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    WorldPacket data(SMSG_PLAY_SPELL_VISUAL, 8 + 4);
    data << m_caster->GetObjectGuid();
    data << uint32(SpellID);                                // spell visual id?
    ((Player*)m_caster)->GetSession()->SendPacket(&data);
}

void Spell::TakeCastItem()
{
    if(!m_CastItem || m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    // not remove cast item at triggered spell (equipping, weapon damage, etc)
    if(m_IsTriggeredSpell && !(m_targets.m_targetMask & TARGET_FLAG_TRADE_ITEM))
        return;

    ItemPrototype const *proto = m_CastItem->GetProto();

    if(!proto)
    {
        // This code is to avoid a crash
        // I'm not sure, if this is really an error, but I guess every item needs a prototype
        sLog.outError("Cast item (%s) has no item prototype", m_CastItem->GetGuidStr().c_str());
        return;
    }

    bool expendable = false;
    bool withoutCharges = false;

    for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        if (proto->Spells[i].SpellId)
        {
            // item has limited charges
            if (proto->Spells[i].SpellCharges)
            {
                if (proto->Spells[i].SpellCharges < 0 && !(proto->ExtraFlags & ITEM_EXTRA_NON_CONSUMABLE))
                    expendable = true;

                int32 charges = m_CastItem->GetSpellCharges(i);

                // item has charges left
                if (charges)
                {
                    (charges > 0) ? --charges : ++charges;  // abs(charges) less at 1 after use
                    if (proto->Stackable < 2)
                        m_CastItem->SetSpellCharges(i, charges);
                    m_CastItem->SetState(ITEM_CHANGED, (Player*)m_caster);
                }

                // all charges used
                withoutCharges = (charges == 0);
            }
        }
    }

    if (expendable && withoutCharges)
    {
        uint32 count = 1;
        ((Player*)m_caster)->DestroyItemCount(m_CastItem, count, true);

        // prevent crash at access to deleted m_targets.getItemTarget
        ClearCastItem();
    }
}

void Spell::TakePower()
{
    if(m_CastItem || m_triggeredByAuraSpell)
        return;

    // health as power used
    if(m_spellInfo->powerType == POWER_HEALTH)
    {
        m_caster->DealDamage(m_caster,m_powerCost,NULL,SELF_DAMAGE,SPELL_SCHOOL_MASK_NORMAL,NULL,false);
        return;
    }

    if(m_spellInfo->powerType >= MAX_POWERS)
    {
        sLog.outError("Spell::TakePower: Unknown power type '%d'", m_spellInfo->powerType);
        return;
    }

    Powers powerType = Powers(m_spellInfo->powerType);

    m_caster->ModifyPower(powerType, -(int32)m_powerCost);

    // Set the five second timer
    if (powerType == POWER_MANA && m_powerCost > 0)
        m_caster->SetLastManaUse();
}

void Spell::TakeReagents()
{
    if (m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    if (IgnoreItemRequirements())                           // reagents used in triggered spell removed by original spell or don't must be removed.
        return;

    Player* p_caster = (Player*)m_caster;
    if (p_caster->CanNoReagentCast(m_spellInfo))
        return;

    for(uint32 x = 0; x < MAX_SPELL_REAGENTS; ++x)
    {
        if(m_spellInfo->Reagent[x] <= 0)
            continue;

        uint32 itemid = m_spellInfo->Reagent[x];
        uint32 itemcount = m_spellInfo->ReagentCount[x];

        // if CastItem is also spell reagent
        if (m_CastItem)
        {
            ItemPrototype const *proto = m_CastItem->GetProto();
            if( proto && proto->ItemId == itemid )
            {
                for(int s = 0; s < MAX_ITEM_PROTO_SPELLS; ++s)
                {
                    // CastItem will be used up and does not count as reagent
                    int32 charges = m_CastItem->GetSpellCharges(s);
                    if (proto->Spells[s].SpellCharges < 0 && abs(charges) < 2)
                    {
                        ++itemcount;
                        break;
                    }
                }

                m_CastItem = NULL;
            }
        }

        // if getItemTarget is also spell reagent
        if (m_targets.getItemTargetEntry() == itemid)
            m_targets.setItemTarget(NULL);

        p_caster->DestroyItemCount(itemid, itemcount, true);
    }
}

void Spell::HandleThreatSpells()
{
    if (m_UniqueTargetInfo.empty())
        return;

    SpellThreatEntry const* threatEntry = sSpellMgr.GetSpellThreatEntry(m_spellInfo->Id);

    if (!threatEntry || (!threatEntry->threat && threatEntry->ap_bonus == 0.0f))
        return;

    float threat = threatEntry->threat;
    if (threatEntry->ap_bonus != 0.0f)
        threat += threatEntry->ap_bonus * m_caster->GetTotalAttackPowerValue(GetWeaponAttackType(m_spellInfo));

    bool positive = true;
    uint8 effectMask = 0;
    for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
        if (m_spellInfo->Effect[i])
            effectMask |= (1<<i);

    if (m_negativeEffectMask & effectMask)
    {
        // can only handle spells with clearly defined positive/negative effect, check at spell_threat loading probably not perfect
        // so abort when only some effects are negative.
        if ((m_negativeEffectMask & effectMask) != effectMask)
        {
            DEBUG_FILTER_LOG(LOG_FILTER_SPELL_CAST, "Spell %u, rank %u, is not clearly positive or negative, ignoring bonus threat", m_spellInfo->Id, sSpellMgr.GetSpellRank(m_spellInfo->Id));
            return;
        }
        positive = false;
    }

    // before 2.0.1 threat from positive effects not dependent from targets amount
    // The threat from Demoralizing Shout and Demoralizing Roar should not be split between affected mobs.
    if (!positive &&
            m_spellInfo->Id != 1160 && m_spellInfo->Id != 6190 && m_spellInfo->Id != 1554 && m_spellInfo->Id != 11555 && m_spellInfo->Id != 11556 &&
            m_spellInfo->Id != 99 && m_spellInfo->Id != 1735 && m_spellInfo->Id != 9490 && m_spellInfo->Id != 9747 && m_spellInfo->Id != 9898)
        threat /= m_UniqueTargetInfo.size();

    for (TargetList::const_iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        if (ihit->missCondition != SPELL_MISS_NONE)
            continue;

        Unit* target = m_caster->GetObjectGuid() == ihit->targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, ihit->targetGUID);
        if (!target)
            continue;

        // positive spells distribute threat among all units that are in combat with target, like healing
        if (positive)
        {
            target->getHostileRefManager().threatAssist(m_caster /*real_caster ??*/, threat, m_spellInfo);
        }
        // for negative spells threat gets distributed among affected targets
        else
        {
            if (!target->CanHaveThreatList())
                continue;

            if(strcmp("Holy Nova", *m_spellInfo->SpellName) == 0) // Holy nova adds no threat.
            {
                target->AddThreat(m_caster, 0, false, GetSpellSchoolMask(m_spellInfo), m_spellInfo);
            }
            else
            {
                target->AddThreat(m_caster, threat, false, GetSpellSchoolMask(m_spellInfo), m_spellInfo);
            }

        }
    }

    DEBUG_FILTER_LOG(LOG_FILTER_SPELL_CAST, "Spell %u added an additional %f threat for %s %u target(s)", m_spellInfo->Id, threat, positive ? "assisting" : "harming", uint32(m_UniqueTargetInfo.size()));
}

void Spell::HandleEffects(Unit *pUnitTarget,Item *pItemTarget,GameObject *pGOTarget,SpellEffectIndex i, float DamageMultiplier)
{
    unitTarget = pUnitTarget;
    itemTarget = pItemTarget;
    gameObjTarget = pGOTarget;

    uint8 eff = m_spellInfo->Effect[i];

    damage = int32(CalculateDamage(i, unitTarget) * DamageMultiplier);

    DEBUG_FILTER_LOG(LOG_FILTER_SPELL_CAST, "Spell %u Effect%d : %u Targets: %s, %s, %s",
                     m_spellInfo->Id, i, eff,
                     unitTarget ? unitTarget->GetGuidStr().c_str() : "-",
                     itemTarget ? itemTarget->GetGuidStr().c_str() : "-",
                     gameObjTarget ? gameObjTarget->GetGuidStr().c_str() : "-");

    Creature* victim_creature = dynamic_cast<Creature*>(unitTarget);
    if (victim_creature && !IsPositiveSpell(m_spellInfo) && !(m_spellInfo->AttributesEx3 & SPELL_ATTR_EX3_NO_INITIAL_AGGRO))
    {
        victim_creature->SetPlayerHitResetTimer(12000); // Value verified in MoP retail to be 12 seconds.
        victim_creature->SetCombatStartPosition(victim_creature->GetPositionX(), victim_creature->GetPositionY(), victim_creature->GetPositionZ()); // Update the combat starting pos for the range check.
    }


    if(eff < TOTAL_SPELL_EFFECTS)
    {
        (*this.*SpellEffects[eff])(i);
        sMod.spellEffect(this, eff , i);  // extra for prepare
    }
    else
    {
        sLog.outError("WORLD: Spell FX %d > TOTAL_SPELL_EFFECTS ", eff);
    }
}

void Spell::AddTriggeredSpell( uint32 spellId )
{
    SpellEntry const *spellInfo = sSpellStore.LookupEntry(spellId );

    if(!spellInfo)
    {
        sLog.outError("Spell::AddTriggeredSpell: unknown spell id %u used as triggred spell for spell %u)", spellId, m_spellInfo->Id);
        return;
    }

    m_TriggerSpells.push_back(spellInfo);
}

void Spell::AddPrecastSpell( uint32 spellId )
{
    SpellEntry const *spellInfo = sSpellStore.LookupEntry(spellId );

    if(!spellInfo)
    {
        sLog.outError("Spell::AddPrecastSpell: unknown spell id %u used as pre-cast spell for spell %u)", spellId, m_spellInfo->Id);
        return;
    }

    m_preCastSpells.push_back(spellInfo);
}

void Spell::CastTriggerSpells()
{
    for(SpellInfoList::const_iterator si = m_TriggerSpells.begin(); si != m_TriggerSpells.end(); ++si)
    {
        Spell* spell = new Spell(m_caster, (*si), true, m_originalCasterGUID);
        spell->prepare(&m_targets);                         // use original spell original targets
    }
}

void Spell::CastPreCastSpells(Unit* target)
{
    for(SpellInfoList::const_iterator si = m_preCastSpells.begin(); si != m_preCastSpells.end(); ++si)
        m_caster->CastSpell(target, (*si), true, m_CastItem);
}

SpellCastResult Spell::CheckCast(bool strict)
{
    // check cooldowns to prevent cheating (ignore passive spells, that client side visual only)
    if (m_caster->GetTypeId()==TYPEID_PLAYER && !(m_spellInfo->Attributes & SPELL_ATTR_PASSIVE) &&
            ((Player*)m_caster)->HasSpellCooldown(m_spellInfo->Id))
    {
        if(m_triggeredByAuraSpell)
            return SPELL_FAILED_DONT_REPORT;
        else
            return SPELL_FAILED_NOT_READY;
    }

    // check global cooldown
    if (strict && !m_IsTriggeredSpell && HasGlobalCooldown())
        return SPELL_FAILED_NOT_READY;

    // only allow triggered spells if at an ended battleground
    if (!m_IsTriggeredSpell && m_caster->GetTypeId() == TYPEID_PLAYER)
        if(BattleGround * bg = ((Player*)m_caster)->GetBattleGround())
            if(bg->GetStatus() == STATUS_WAIT_LEAVE)
                return SPELL_FAILED_DONT_REPORT;

    if (!m_IsTriggeredSpell && IsNonCombatSpell(m_spellInfo) &&
            m_caster->isInCombat())
        return SPELL_FAILED_AFFECTING_COMBAT;

    if (m_caster->GetTypeId() == TYPEID_PLAYER && !((Player*)m_caster)->isGameMaster() &&
            sWorld.getConfig(CONFIG_BOOL_VMAP_INDOOR_CHECK) &&
            VMAP::VMapFactory::createOrGetVMapManager()->isLineOfSightCalcEnabled())
    {
        if (m_spellInfo->Attributes & SPELL_ATTR_OUTDOORS_ONLY &&
                !m_caster->GetTerrain()->IsOutdoors(m_caster->GetPositionX(), m_caster->GetPositionY(), m_caster->GetPositionZ()))
            return SPELL_FAILED_ONLY_OUTDOORS;

        if(m_spellInfo->Attributes & SPELL_ATTR_INDOORS_ONLY &&
                m_caster->GetTerrain()->IsOutdoors(m_caster->GetPositionX(), m_caster->GetPositionY(), m_caster->GetPositionZ()))
            return SPELL_FAILED_ONLY_INDOORS;
    }
    // only check at first call, Stealth auras are already removed at second call
    // for now, ignore triggered spells
    if (strict && !m_IsTriggeredSpell)
    {
        // Cannot be used in this stance/form
        SpellCastResult shapeError = GetErrorAtShapeshiftedCast(m_spellInfo, m_caster->GetShapeshiftForm());
        if(shapeError != SPELL_CAST_OK)
            return shapeError;

        // Do not allow shapeshift when the warrior has Nefarian's debuff.
        if (m_caster->HasAura(23397))
            for (int i = 0; i < MAX_EFFECT_INDEX; i++)
                if (m_spellInfo->EffectApplyAuraName[i] == SPELL_AURA_MOD_SHAPESHIFT)
                    return SPELL_FAILED_NOT_SHAPESHIFT;

        if ((m_spellInfo->Attributes & SPELL_ATTR_ONLY_STEALTHED) && !(m_caster->HasStealthAura()))
            return SPELL_FAILED_ONLY_STEALTHED;
    }

    // caster state requirements
    if(m_spellInfo->CasterAuraState && !m_caster->HasAuraState(AuraState(m_spellInfo->CasterAuraState)))
        return SPELL_FAILED_CANT_DO_THAT_YET;

    if (m_caster->GetTypeId() == TYPEID_PLAYER)
    {
        // cancel autorepeat spells if cast start when moving
        // (not wand currently autorepeat cast delayed to moving stop anyway in spell update code)
        if (((Player*)m_caster)->isMoving() )
        {
            // skip stuck spell to allow use it in falling case and apply spell limitations at movement
            if ((!((Player*)m_caster)->m_movementInfo.HasMovementFlag(MOVEFLAG_FALLING_FAR) || m_spellInfo->Effect[EFFECT_INDEX_0] != SPELL_EFFECT_STUCK) &&
                    (IsAutoRepeat() || (m_spellInfo->AuraInterruptFlags & AURA_INTERRUPT_FLAG_NOT_SEATED) != 0))
                return SPELL_FAILED_MOVING;
        }

        if (!m_IsTriggeredSpell && NeedsComboPoints(m_spellInfo) &&
                (!m_targets.getUnitTarget() || m_targets.getUnitTarget()->GetObjectGuid() != ((Player*)m_caster)->GetComboTargetGuid()))
            // warrior not have real combo-points at client side but use this way for mark allow Overpower use
            return m_caster->getClass() == CLASS_WARRIOR ? SPELL_FAILED_CANT_DO_THAT_YET : SPELL_FAILED_NO_COMBO_POINTS;
    }

    if(Unit *target = m_targets.getUnitTarget())
    {
        // Swiftmend
        if (m_spellInfo->Id == 18562)                       // future versions have special aura state for this
        {
            if (!target->GetAura(SPELL_AURA_PERIODIC_HEAL,SPELLFAMILY_DRUID,UI64LIT(0x50)))
                return SPELL_FAILED_TARGET_AURASTATE;
        }

        // Fill possible dispel list
        bool isDispell = false;
        bool isEmpty = true;

        // As of Patch 1.10.0, dispel effects now check if there is something to dispel first
        for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            // Dispell Magic
            switch (m_spellInfo->Effect[i])
            {
            case SPELL_EFFECT_DISPEL:
            {
                // It is a dispell spell
                isDispell = true;

                // Create dispel mask by dispel type
                uint32 dispel_type = m_spellInfo->EffectMiscValue[i];
                uint32 dispelMask = GetDispellMask(DispelType(dispel_type));
                Unit::SpellAuraHolderMap const& auras = target->GetSpellAuraHolderMap();
                for (Unit::SpellAuraHolderMap::const_iterator itr = auras.begin(); itr != auras.end(); ++itr)
                {
                    SpellAuraHolder* holder = itr->second;
                    uint32 disp = (1 << holder->GetSpellProto()->Dispel);
                    if (disp & dispelMask)
                    {
                        if (holder->GetSpellProto()->Dispel == DISPEL_MAGIC)
                        {
                            bool positive = true;
                            if (!holder->IsPositive())
                            {
                                positive = false;
                            }
                            else
                            {
                                positive = (holder->GetSpellProto()->AttributesEx & SPELL_ATTR_EX_NEGATIVE) == 0;
                            }

                            // do not remove positive auras if friendly target
                            //               negative auras if non-friendly target
                            if (positive && target->GetTypeId() == TYPEID_PLAYER) // Handle player targets separately since the PvP-tag needs to be checked.
                            {
                                Player* pTarget = dynamic_cast<Player*>(target);
                                Player* pCaster = dynamic_cast<Player*>(m_caster);
                                
                                // If the players belong to different factions beneficial spells should be dispelable.
                                if (pTarget && pCaster && (pTarget->TeamForRace(pTarget->getRace()) != pCaster->TeamForRace(pCaster->getRace()) || pCaster->IsInDuelWith(pTarget)) && pTarget->IsPvP())
                                {
                                    isEmpty = false;
                                    break;
                                }
                                else
                                    continue;
                            }
                            else if (positive == target->IsFriendlyTo(m_caster))			// if target is another faction and pvp-enabled while caster isn't pvp, should be able to dispel.
                                continue;
                        }
                        isEmpty = false;
                        break;
                    }
                }
                break;
            }
            }
        }

        // If there's nothing to dispel the over time cures should still be allowed to be applied.
        if (isDispell && isEmpty)
        {
            // abolish disease initial cast
            // abolish Disease effect
            // abolish poison initial cast
            // abolish poison effect
            Totem* pCasterT = dynamic_cast<Totem*>(m_caster);

            // Make sure that the dispel check is only applied on player targets. Warriors' Shield Bash should also be an exception.
            // 24406 - Improved Mend Pet is also excepted.
            if(m_caster && m_caster->getClass() != CLASS_WARRIOR && target->GetTypeId() != TYPEID_UNIT && !pCasterT )
            {
                if (m_spellInfo->Id != 552 && m_spellInfo->Id != 10872 &&
                    m_spellInfo->Id != 2893 && m_spellInfo->Id != 3137 &&
                    m_spellInfo->Id != 24406)
                    return SPELL_FAILED_NOTHING_TO_DISPEL;
            }
        }

        if (!m_IsTriggeredSpell && IsDeathOnlySpell(m_spellInfo) && target->isAlive())
            return SPELL_FAILED_TARGET_NOT_DEAD;

        bool non_caster_target = target != m_caster && !IsSpellWithCasterSourceTargetsOnly(m_spellInfo);

        if(non_caster_target)
        {
            // Not allow casting on flying player
            if (target->IsTaxiFlying())
                return SPELL_FAILED_BAD_TARGETS;

            //Range index 13 is "everywhere" and shouldn't have LOS restrictions
            if(!m_IsTriggeredSpell && m_spellInfo->rangeIndex != 13 && VMAP::VMapFactory::checkSpellForLoS(m_spellInfo->Id) && !m_caster->IsWithinLOSInMap(target))
                return SPELL_FAILED_LINE_OF_SIGHT;

            // auto selection spell rank implemented in WorldSession::HandleCastSpellOpcode
            // this case can be triggered if rank not found (too low-level target for first rank)
            if (m_caster->GetTypeId() == TYPEID_PLAYER && !m_CastItem && !m_IsTriggeredSpell)
            {
                // spell expected to be auto-downranking in cast handle, so must be same
                if (m_spellInfo != sSpellMgr.SelectAuraRankForLevel(m_spellInfo, target->getLevel()))
                    return SPELL_FAILED_LOWLEVEL;
            }
        }
        else if (m_caster == target)
        {
            if (m_caster->GetTypeId() == TYPEID_PLAYER && m_caster->IsInWorld())
            {
                // Additional check for some spells
                // If 0 spell effect empty - client not send target data (need use selection)
                // TODO: check it on next client version
                if (m_targets.m_targetMask == TARGET_FLAG_SELF &&
                        m_spellInfo->EffectImplicitTargetA[EFFECT_INDEX_1] == TARGET_CHAIN_DAMAGE)
                {
                    target = m_caster->GetMap()->GetUnit(((Player *)m_caster)->GetSelectionGuid());
                    if (!target)
                        return SPELL_FAILED_BAD_TARGETS;

                    m_targets.setUnitTarget(target);
                }
            }

            // Some special spells with non-caster only mode

            // Fire Shield
            if (m_spellInfo->SpellFamilyName == SPELLFAMILY_WARLOCK &&
                    m_spellInfo->SpellIconID == 16)
                return SPELL_FAILED_BAD_TARGETS;
        }

        // Revive Pet shouldn't check if the pet is actually in the world
        // if the pet is dead and despawned it should still need to be revived.
        if (m_spellInfo->Id != 982)
        {
            // check pet presents
            for(int j = 0; j < MAX_EFFECT_INDEX; ++j)
            {
                if(m_spellInfo->EffectImplicitTargetA[j] == TARGET_PET)
                {
                    if(!m_caster->GetPet())
                    {
                        if(m_triggeredByAuraSpell)              // not report pet not existence for triggered spells
                            return SPELL_FAILED_DONT_REPORT;
                        else
                            return SPELL_FAILED_NO_PET;
                    }
                    break;
                }
            }
        }

        //check creature type
        //ignore self casts (including area casts when caster selected as target)
        if(non_caster_target)
        {
            if(!CheckTargetCreatureType(target))
            {
                if(target->GetTypeId() == TYPEID_PLAYER)
                    return SPELL_FAILED_TARGET_IS_PLAYER;
                else
                    return SPELL_FAILED_BAD_TARGETS;
            }
        }

        if(non_caster_target)
        {
            // simple cases
            bool explicit_target_mode = false;
            bool target_hostile = false;
            bool target_hostile_checked = false;
            bool target_friendly = false;
            bool target_friendly_checked = false;
            for(int k = 0; k < MAX_EFFECT_INDEX;  ++k)
            {
                if (IsExplicitPositiveTarget(m_spellInfo->EffectImplicitTargetA[k]))
                {
                    if (!target_hostile_checked)
                    {
                        target_hostile_checked = true;
                        target_hostile = m_caster->IsHostileTo(target);
                    }

                    if(target_hostile)
                        return SPELL_FAILED_BAD_TARGETS;

                    explicit_target_mode = true;
                }
                else if (IsExplicitNegativeTarget(m_spellInfo->EffectImplicitTargetA[k]))
                {
                    if (!target_friendly_checked)
                    {
                        target_friendly_checked = true;
                        target_friendly = m_caster->IsFriendlyTo(target);
                    }

                    if(target_friendly)
                        return SPELL_FAILED_BAD_TARGETS;

                    explicit_target_mode = true;
                }
            }
            // TODO: this check can be applied and for player to prevent cheating when IsPositiveSpell will return always correct result.
            // check target for pet/charmed casts (not self targeted), self targeted cast used for area effects and etc
            if (!explicit_target_mode && m_caster->GetTypeId() == TYPEID_UNIT && m_caster->GetCharmerOrOwnerGuid() && m_spellInfo->SpellIconID != 47) // Exclude Devour Magic from this check.
            {
                // check correctness positive/negative cast target (pet cast real check and cheating check)
                if(IsPositiveSpell(m_spellInfo->Id))
                {
                    if (!target_hostile_checked)
                    {
                        target_hostile_checked = true;
                        target_hostile = m_caster->IsHostileTo(target);
                    }

                    if(target_hostile)
                        return SPELL_FAILED_BAD_TARGETS;
                }
                else
                {
                    if (!target_friendly_checked)
                    {
                        target_friendly_checked = true;
                        target_friendly = m_caster->IsFriendlyTo(target);
                    }

                    if(target_friendly)
                        return SPELL_FAILED_BAD_TARGETS;
                }
            }
        }

        if(IsPositiveSpell(m_spellInfo->Id))
        {
            if(target->IsImmuneToSpell(m_spellInfo))
                return SPELL_FAILED_TARGET_AURASTATE;
        }

        //Must be behind the target.
        if( ((m_spellInfo->AttributesEx2 == 0x100000 && (m_spellInfo->AttributesEx & 0x200) == 0x200) ||
                (m_spellInfo->SpellFamilyName == SPELLFAMILY_GENERIC && m_spellInfo->SpellIconID == 243 && m_spellInfo->SpellVisual == 611)) && target->HasInArc(M_PI_F, m_caster) )
        {

            //If it's an NPC trainer casting the learn spell on a player, it's acceptable to be in front.
            if (!m_caster->isTrainer())
            {
                SendInterrupted(2);
                return SPELL_FAILED_NOT_BEHIND;
            }
        }

        //Target must be facing you.
        if((m_spellInfo->Attributes == 0x150010) && !target->HasInArc(M_PI_F, m_caster) )
        {
            SendInterrupted(2);
            return SPELL_FAILED_NOT_INFRONT;
        }

        // check if target is in combat
        if (non_caster_target && (m_spellInfo->AttributesEx & SPELL_ATTR_EX_NOT_IN_COMBAT_TARGET) && target->isInCombat())
            return SPELL_FAILED_TARGET_AFFECTING_COMBAT;
    }
    // zone check
    uint32 zone, area;
    m_caster->GetZoneAndAreaId(zone, area);

    SpellCastResult locRes= sSpellMgr.GetSpellAllowedInLocationError(m_spellInfo, m_caster->GetMapId(), zone, area,
                            m_caster->GetCharmerOrOwnerPlayerOrPlayerItself());
    if (locRes != SPELL_CAST_OK)
        return locRes;

    // not let players cast spells at mount (and let do it to creatures)
    if (m_caster->IsMounted() && m_caster->GetTypeId()==TYPEID_PLAYER && !m_IsTriggeredSpell &&
            !IsPassiveSpell(m_spellInfo) && !(m_spellInfo->Attributes & SPELL_ATTR_CASTABLE_WHILE_MOUNTED))
    {
        if (m_caster->IsTaxiFlying())
            return SPELL_FAILED_NOT_ON_TAXI;
        else
            return SPELL_FAILED_NOT_MOUNTED;
    }

    // always (except passive spells) check items (focus object can be required for any type casts)
    if (!IsPassiveSpell(m_spellInfo))
    {
        SpellCastResult castResult = CheckItems();
        if(castResult != SPELL_CAST_OK)
            return castResult;
    }

    // Database based targets from spell_target_script
    if (m_UniqueTargetInfo.empty())                         // skip second CheckCast apply (for delayed spells for example)
    {
        for(int j = 0; j < MAX_EFFECT_INDEX; ++j)
        {
            if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT ||
                    (m_spellInfo->EffectImplicitTargetB[j] == TARGET_SCRIPT && m_spellInfo->EffectImplicitTargetA[j] != TARGET_SELF) ||
                    m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT_COORDINATES ||
                    m_spellInfo->EffectImplicitTargetB[j] == TARGET_SCRIPT_COORDINATES ||
                    m_spellInfo->EffectImplicitTargetA[j] == TARGET_FOCUS_OR_SCRIPTED_GAMEOBJECT)
            {
                SpellRangeEntry const* srange = sSpellRangeStore.LookupEntry(m_spellInfo->rangeIndex);
                float range = GetSpellMaxRange(srange);

                Creature* targetExplicit = NULL;            // used for cases where a target is provided (by script for example)
                Creature* creatureScriptTarget = NULL;
                GameObject* goScriptTarget = NULL;

                SpellScriptTargetBounds bounds = sSpellMgr.GetSpellScriptTargetBounds(m_spellInfo->Id);

                if (bounds.first == bounds.second)
                {
                    switch (m_spellInfo->Id)
                    {
                    case 24973: //Clean stink bomb
                        if (j == EFFECT_INDEX_0)
                        {
                            //Find the stink bomb itself
                            MaNGOS::NearestGameObjectEntryInObjectRangeCheck go_check(*m_caster, 180449, range);
                            MaNGOS::GameObjectLastSearcher<MaNGOS::NearestGameObjectEntryInObjectRangeCheck> checker(goScriptTarget, go_check);
                            Cell::VisitGridObjects(m_caster, checker, range);
                        } else if (j == EFFECT_INDEX_1)
                        {
                            //Find the stink cloud
                            MaNGOS::NearestGameObjectEntryInObjectRangeCheck go_check(*m_caster, 180450, range);
                            MaNGOS::GameObjectLastSearcher<MaNGOS::NearestGameObjectEntryInObjectRangeCheck> checker(goScriptTarget, go_check);
                            Cell::VisitGridObjects(m_caster, checker, range);
                        }
                    default: //Only the above exceptions should not have a target entry in the db
                        if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT || m_spellInfo->EffectImplicitTargetB[j] == TARGET_SCRIPT)
                            sLog.outErrorDb("Spell entry %u, effect %i has EffectImplicitTargetA/EffectImplicitTargetB = TARGET_SCRIPT, but creature are not defined in `spell_script_target`", m_spellInfo->Id, j);

                        if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT_COORDINATES || m_spellInfo->EffectImplicitTargetB[j] == TARGET_SCRIPT_COORDINATES)
                            sLog.outErrorDb("Spell entry %u, effect %i has EffectImplicitTargetA/EffectImplicitTargetB = TARGET_SCRIPT_COORDINATES, but gameobject or creature are not defined in `spell_script_target`", m_spellInfo->Id, j);

                        if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_FOCUS_OR_SCRIPTED_GAMEOBJECT)
                            sLog.outErrorDb("Spell entry %u, effect %i has EffectImplicitTargetA/EffectImplicitTargetB = TARGET_FOCUS_OR_SCRIPTED_GAMEOBJECT, but gameobject are not defined in `spell_script_target`", m_spellInfo->Id, j);
                    }
                }

                for(SpellScriptTarget::const_iterator i_spellST = bounds.first; i_spellST != bounds.second; ++i_spellST)
                {
                    switch(i_spellST->second.type)
                    {
                    case SPELL_TARGET_TYPE_GAMEOBJECT:
                    {
                        GameObject* p_GameObject = NULL;

                        if (i_spellST->second.targetEntry)
                        {
                            MaNGOS::NearestGameObjectEntryInObjectRangeCheck go_check(*m_caster, i_spellST->second.targetEntry, range);
                            MaNGOS::GameObjectLastSearcher<MaNGOS::NearestGameObjectEntryInObjectRangeCheck> checker(p_GameObject, go_check);
                            Cell::VisitGridObjects(m_caster, checker, range);

                            if (p_GameObject)
                            {
                                // remember found target and range, next attempt will find more near target with another entry
                                creatureScriptTarget = NULL;
                                goScriptTarget = p_GameObject;
                                range = go_check.GetLastRange();
                            }
                        }
                        else if (focusObject)           // Focus Object
                        {
                            float frange = m_caster->GetDistance(focusObject);
                            if (range >= frange)
                            {
                                creatureScriptTarget = NULL;
                                goScriptTarget = focusObject;
                                range = frange;
                            }
                        }
                        break;
                    }
                    case SPELL_TARGET_TYPE_CREATURE:
                    case SPELL_TARGET_TYPE_DEAD:
                    default:
                    {
                        Creature *p_Creature = NULL;

                        // check if explicit target is provided and check it up against database valid target entry/state
                        if (Unit* pTarget = m_targets.getUnitTarget())
                        {
                            if (pTarget->GetTypeId() == TYPEID_UNIT && pTarget->GetEntry() == i_spellST->second.targetEntry)
                            {
                                if (i_spellST->second.type == SPELL_TARGET_TYPE_DEAD && ((Creature*)pTarget)->IsCorpse())
                                {
                                    // always use spellMaxRange, in case GetLastRange returned different in a previous pass
                                    if (pTarget->IsWithinDistInMap(m_caster, GetSpellMaxRange(srange)))
                                        targetExplicit = (Creature*)pTarget;
                                }
                                else if (i_spellST->second.type == SPELL_TARGET_TYPE_CREATURE && pTarget->isAlive())
                                {
                                    // always use spellMaxRange, in case GetLastRange returned different in a previous pass
                                    if (pTarget->IsWithinDistInMap(m_caster, GetSpellMaxRange(srange)))
                                        targetExplicit = (Creature*)pTarget;
                                }
                            }
                        }

                        // no target provided or it was not valid, so use closest in range
                        if (!targetExplicit)
                        {
                            MaNGOS::NearestCreatureEntryWithLiveStateInObjectRangeCheck u_check(*m_caster, i_spellST->second.targetEntry, i_spellST->second.type != SPELL_TARGET_TYPE_DEAD, range);
                            MaNGOS::CreatureLastSearcher<MaNGOS::NearestCreatureEntryWithLiveStateInObjectRangeCheck> searcher(p_Creature, u_check);

                            // Visit all, need to find also Pet* objects
                            Cell::VisitAllObjects(m_caster, searcher, range);

                            range = u_check.GetLastRange();
                        }

                        // always prefer provided target if it's valid
                        if (targetExplicit)
                            creatureScriptTarget = targetExplicit;
                        else if (p_Creature)
                            creatureScriptTarget = p_Creature;

                        if (creatureScriptTarget)
                            goScriptTarget = NULL;

                        break;
                    }
                    }
                }

                if (creatureScriptTarget)
                {
                    // store coordinates for TARGET_SCRIPT_COORDINATES
                    if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT_COORDINATES ||
                            m_spellInfo->EffectImplicitTargetB[j] == TARGET_SCRIPT_COORDINATES)
                    {
                        m_targets.setDestination(creatureScriptTarget->GetPositionX(),creatureScriptTarget->GetPositionY(),creatureScriptTarget->GetPositionZ());

                        if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT_COORDINATES && m_spellInfo->Effect[j] != SPELL_EFFECT_PERSISTENT_AREA_AURA)
                            AddUnitTarget(creatureScriptTarget, SpellEffectIndex(j));
                    }
                    // store explicit target for TARGET_SCRIPT
                    else
                    {
                        if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT ||
                                m_spellInfo->EffectImplicitTargetB[j] == TARGET_SCRIPT)
                            AddUnitTarget(creatureScriptTarget, SpellEffectIndex(j));
                    }
                }
                else if (goScriptTarget)
                {
                    // store coordinates for TARGET_SCRIPT_COORDINATES
                    if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT_COORDINATES ||
                            m_spellInfo->EffectImplicitTargetB[j] == TARGET_SCRIPT_COORDINATES)
                    {
                        m_targets.setDestination(goScriptTarget->GetPositionX(),goScriptTarget->GetPositionY(),goScriptTarget->GetPositionZ());

                        if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_SCRIPT_COORDINATES && m_spellInfo->Effect[j] != SPELL_EFFECT_PERSISTENT_AREA_AURA)
                            AddGOTarget(goScriptTarget, SpellEffectIndex(j));
                    }
                    // store explicit target for TARGET_FOCUS_OR_SCRIPTED_GAMEOBJECT
                    else
                    {
                        if (m_spellInfo->EffectImplicitTargetA[j] == TARGET_FOCUS_OR_SCRIPTED_GAMEOBJECT ||
                                m_spellInfo->EffectImplicitTargetB[j] == TARGET_FOCUS_OR_SCRIPTED_GAMEOBJECT)
                            AddGOTarget(goScriptTarget, SpellEffectIndex(j));
                    }
                }
                //Missing DB Entry or targets for this spellEffect.
                else
                {
                    // not report target not existence for triggered spells
                    if (m_triggeredByAuraSpell || m_IsTriggeredSpell)
                        return SPELL_FAILED_DONT_REPORT;
                    else
                        return SPELL_FAILED_OUT_OF_RANGE;
                }
            }
        }
    }

    if(!m_IsTriggeredSpell)
    {
        if(!m_triggeredByAuraSpell)
        {
            SpellCastResult castResult = CheckRange(strict);
            if(castResult != SPELL_CAST_OK)
                return castResult;
        }
    }

    {
        SpellCastResult castResult = CheckPower();
        if(castResult != SPELL_CAST_OK)
            return castResult;
    }

    if(!m_IsTriggeredSpell)                                 // triggered spell not affected by stun/etc
    {
        SpellCastResult castResult = CheckCasterAuras();
        if(castResult != SPELL_CAST_OK)
            return castResult;
    }

    if (m_spellInfo->SpellFamilyName == SPELLFAMILY_ROGUE)
    {
        if (!m_caster->HasInArc(M_PI_F, m_targets.getUnitTarget())
                && ((m_spellInfo->SpellVisual == 155 && m_spellInfo->SpellIconID == 243)    //backstab
                    ||  (m_spellInfo->SpellVisual == 757 && m_spellInfo->SpellIconID == 498)    //garrote
                    ||  (m_spellInfo->SpellVisual == 679 && m_spellInfo->SpellIconID == 499)    //kidney shot
                    ||  (m_spellInfo->SpellVisual == 253 && m_spellInfo->SpellIconID == 130)    //sinister strike
                    ||  (m_spellInfo->SpellVisual == 90 && m_spellInfo->SpellIconID == 246)     //kick
                    ||  (m_spellInfo->SpellVisual == 256 && m_spellInfo->SpellIconID == 245)    //gouge
                    ||  (m_spellInfo->SpellVisual == 266 && m_spellInfo->SpellIconID == 244)    //cheap shot
                    ||  (m_spellInfo->SpellVisual == 738 && m_spellInfo->SpellIconID == 539)    //feint
                    ||  (m_spellInfo->SpellVisual == 250 && m_spellInfo->SpellIconID == 500)    //rupture
                    ||  (m_spellInfo->SpellVisual == 671 && m_spellInfo->SpellIconID == 514)    //eviscerate
                    ||  (m_spellInfo->SpellVisual == 257 && m_spellInfo->SpellIconID == 249)    //sap
                    ||  (m_spellInfo->SpellVisual == 3441 && m_spellInfo->SpellIconID == 563)   //expose armor
                    ||  (m_spellInfo->SpellVisual == 155 && m_spellInfo->SpellIconID == 856)    //ambush
                    ||  (m_spellInfo->SpellVisual == 257 && m_spellInfo->SpellIconID == 277)    //pick pocket
                    ||  (m_spellInfo->SpellVisual == 3799 && m_spellInfo->SpellIconID == 278)   //riposte
                    ||  (m_spellInfo->SpellVisual == 4159 && m_spellInfo->SpellIconID == 596)   //ghostly strike
                    ||  (m_spellInfo->SpellVisual == 5119 && m_spellInfo->SpellIconID == 153))) //hemorrhage
            return SPELL_FAILED_UNIT_NOT_INFRONT;
    }
    else if (m_spellInfo->SpellFamilyName == SPELLFAMILY_WARRIOR)
    {
        if (!m_caster->HasInArc(M_PI_F, m_targets.getUnitTarget())
                && ((m_spellInfo->SpellVisual == 42 && m_spellInfo->SpellIconID == 280)     //shield bash
                    ||  (m_spellInfo->SpellVisual == 3443 && m_spellInfo->SpellIconID == 23)    //hamstring
                    ||  (m_spellInfo->SpellVisual == 34 && m_spellInfo->SpellIconID == 24)      //taunt
                    ||  (m_spellInfo->SpellVisual == 398 && m_spellInfo->SpellIconID == 560)    //disarm
                    ||  (m_spellInfo->SpellVisual == 39 && m_spellInfo->SpellIconID == 1477)    //mocking blow
                    ||  (m_spellInfo->SpellVisual == 372 && m_spellInfo->SpellIconID == 245)    //rend
                    ||  (m_spellInfo->SpellVisual == 1165 && m_spellInfo->SpellIconID == 559)   //slam
                    ||  (m_spellInfo->SpellVisual == 250 && m_spellInfo->SpellIconID == 1648)   //execute
                    ||  (m_spellInfo->SpellVisual == 1023 && m_spellInfo->SpellIconID == 756)   //pummel
                    ||  (m_spellInfo->SpellVisual == 342 && m_spellInfo->SpellIconID == 562)    //revenge
                    ||  (m_spellInfo->SpellVisual == 39 && m_spellInfo->SpellIconID == 26)      //overpower
                    ||  (m_spellInfo->SpellVisual == 406 && m_spellInfo->SpellIconID == 565)    //sunder armor
                    ||  (m_spellInfo->SpellVisual == 39 && m_spellInfo->SpellIconID == 564)     //mortal strike
                    ||  (m_spellInfo->SpellVisual == 2719 && m_spellInfo->SpellIconID == 25)    //concussion blow
                    ||  (m_spellInfo->SpellVisual == 372 && m_spellInfo->SpellIconID == 38)     //bloodthirst
                    ||  (m_spellInfo->SpellVisual == 42 && m_spellInfo->SpellIconID == 413)))   //shield slam
            return SPELL_FAILED_UNIT_NOT_INFRONT;
    }

    for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        // for effects of spells that have only one target
        switch(m_spellInfo->Effect[i])
        {

        case SPELL_EFFECT_DUMMY:
        {
            if (m_spellInfo->SpellIconID == 1648)       // Execute
            {
                if(!m_targets.getUnitTarget() || m_targets.getUnitTarget()->GetHealth() > m_targets.getUnitTarget()->GetMaxHealth()*0.2)
                    return SPELL_FAILED_BAD_TARGETS;
            }
            else if (m_spellInfo->Id == 51582)          // Rocket Boots Engaged
            {
                if (m_caster->IsInWater())
                    return SPELL_FAILED_ONLY_ABOVEWATER;
            }
            else if(m_spellInfo->SpellIconID == 156)    // Holy Shock
            {
                // spell different for friends and enemies
                // hart version required facing
                if (m_targets.getUnitTarget() && !m_caster->IsFriendlyTo(m_targets.getUnitTarget()) && !m_caster->HasInArc(M_PI_F, m_targets.getUnitTarget()))
                    return SPELL_FAILED_UNIT_NOT_INFRONT;
            }
            break;
        }
        case SPELL_EFFECT_SCHOOL_DAMAGE:
        {
            // Hammer of Wrath
            if(m_spellInfo->SpellVisual == 7250)
            {
                if (!m_targets.getUnitTarget())
                    return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

                if(m_targets.getUnitTarget()->GetHealth() > m_targets.getUnitTarget()->GetMaxHealth()*0.2)
                    return SPELL_FAILED_BAD_TARGETS;
            }
            // Conflagrate
            else if(m_spellInfo->SpellFamilyName == SPELLFAMILY_WARLOCK && m_spellInfo->SpellFamilyFlags & UI64LIT(0x0000000000000200))
            {
                if (!m_targets.getUnitTarget())
                    return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

                // for caster applied auras only
                bool found = false;
                Unit::AuraList const &mPeriodic = m_targets.getUnitTarget()->GetAurasByType(SPELL_AURA_PERIODIC_DAMAGE);
                for(Unit::AuraList::const_iterator i = mPeriodic.begin(); i != mPeriodic.end(); ++i)
                {
                    if ((*i)->GetSpellProto()->SpellFamilyName == SPELLFAMILY_WARLOCK &&
                            (*i)->GetCasterGuid() == m_caster->GetObjectGuid() &&
                            // Immolate
                            ((*i)->GetSpellProto()->SpellFamilyFlags & UI64LIT(0x0000000000000004)))
                    {
                        found = true;
                        break;
                    }
                }

                if (!found)
                    return SPELL_FAILED_TARGET_AURASTATE;
            }
            break;
        }
        case SPELL_EFFECT_TAMECREATURE:
        {
            // Spell can be triggered, we need to check original caster prior to caster
            Unit* caster = GetAffectiveCaster();
            if (!caster || caster->GetTypeId() != TYPEID_PLAYER ||
                    !m_targets.getUnitTarget() ||
                    m_targets.getUnitTarget()->GetTypeId() == TYPEID_PLAYER)
                return SPELL_FAILED_BAD_TARGETS;

            Player* plrCaster = (Player*)caster;

            bool gmmode = m_triggeredBySpellInfo == NULL;

            if (gmmode && !ChatHandler(plrCaster).FindCommand("npc tame"))
            {
                plrCaster->SendPetTameFailure(PETTAME_UNKNOWNERROR);
                return SPELL_FAILED_DONT_REPORT;
            }

            if(plrCaster->getClass() != CLASS_HUNTER && !gmmode)
            {
                plrCaster->SendPetTameFailure(PETTAME_UNITSCANTTAME);
                return SPELL_FAILED_DONT_REPORT;
            }

            Creature* target = (Creature*)m_targets.getUnitTarget();

            if(target->IsPet() || target->isCharmed())
            {
                plrCaster->SendPetTameFailure(PETTAME_CREATUREALREADYOWNED);
                return SPELL_FAILED_DONT_REPORT;
            }

            if (target->getLevel() > plrCaster->getLevel() && !gmmode)
            {
                plrCaster->SendPetTameFailure(PETTAME_TOOHIGHLEVEL);
                return SPELL_FAILED_DONT_REPORT;
            }

            if (!target->GetCreatureInfo()->isTameable())
            {
                plrCaster->SendPetTameFailure(PETTAME_NOTTAMEABLE);
                return SPELL_FAILED_DONT_REPORT;
            }

            if (plrCaster->GetPetGuid() || plrCaster->GetCharmGuid())
            {
                plrCaster->SendPetTameFailure(PETTAME_ANOTHERSUMMONACTIVE);
                return SPELL_FAILED_DONT_REPORT;
            }

            break;
        }
        case SPELL_EFFECT_LEARN_SPELL:
        {
            if(m_spellInfo->EffectImplicitTargetA[i] != TARGET_PET)
                break;

            Pet* pet = m_caster->GetPet();

            if(!pet)
                return SPELL_FAILED_NO_PET;

            SpellEntry const *learn_spellproto = sSpellStore.LookupEntry(m_spellInfo->EffectTriggerSpell[i]);

            if(!learn_spellproto)
                return SPELL_FAILED_NOT_KNOWN;

            if(!pet->CanTakeMoreActiveSpells(learn_spellproto->Id))
                return SPELL_FAILED_TOO_MANY_SKILLS;

            if(m_spellInfo->spellLevel > pet->getLevel())
                return SPELL_FAILED_LOWLEVEL;

            if(!pet->HasTPForSpell(learn_spellproto->Id))
                return SPELL_FAILED_TRAINING_POINTS;

            break;
        }
        case SPELL_EFFECT_LEARN_PET_SPELL:
        {
            Pet* pet = m_caster->GetPet();

            if(!pet)
                return SPELL_FAILED_NO_PET;

            SpellEntry const *learn_spellproto = sSpellStore.LookupEntry(m_spellInfo->EffectTriggerSpell[i]);

            if(!learn_spellproto)
                return SPELL_FAILED_NOT_KNOWN;

            if(!pet->CanTakeMoreActiveSpells(learn_spellproto->Id))
                return SPELL_FAILED_TOO_MANY_SKILLS;

            if(m_spellInfo->spellLevel > pet->getLevel())
                return SPELL_FAILED_LOWLEVEL;

            if(!pet->HasTPForSpell(learn_spellproto->Id))
                return SPELL_FAILED_TRAINING_POINTS;

            break;
        }
        case SPELL_EFFECT_FEED_PET:
        {
            if (m_caster->GetTypeId() != TYPEID_PLAYER)
                return SPELL_FAILED_BAD_TARGETS;

            Item* foodItem = m_targets.getItemTarget();
            if(!foodItem)
                return SPELL_FAILED_BAD_TARGETS;

            Pet* pet = m_caster->GetPet();

            if(!pet)
                return SPELL_FAILED_NO_PET;

            if(!pet->HaveInDiet(foodItem->GetProto()))
                return SPELL_FAILED_WRONG_PET_FOOD;

            if(!pet->GetCurrentFoodBenefitLevel(foodItem->GetProto()->ItemLevel))
                return SPELL_FAILED_FOOD_LOWLEVEL;

            if(pet->isInCombat())
                return SPELL_FAILED_AFFECTING_COMBAT;

            break;
        }
        case SPELL_EFFECT_POWER_BURN:
        case SPELL_EFFECT_POWER_DRAIN:
        {
            // Can be area effect, Check only for players and not check if target - caster (spell can have multiply drain/burn effects)
            if (m_caster->GetTypeId() == TYPEID_PLAYER)
                if (Unit* target = m_targets.getUnitTarget())
                    if (target != m_caster && int32(target->getPowerType()) != m_spellInfo->EffectMiscValue[i])
                        return SPELL_FAILED_BAD_TARGETS;
            break;
        }
        case SPELL_EFFECT_CHARGE:
        {
            if (m_caster->hasUnitState(UNIT_STAT_ROOT))
                return SPELL_FAILED_ROOTED;

            break;
        }
        case SPELL_EFFECT_SKINNING:
        {
            if (m_caster->GetTypeId() != TYPEID_PLAYER || !m_targets.getUnitTarget() || m_targets.getUnitTarget()->GetTypeId() != TYPEID_UNIT)
                return SPELL_FAILED_BAD_TARGETS;

            if (!m_targets.getUnitTarget()->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SKINNABLE))
                return SPELL_FAILED_TARGET_UNSKINNABLE;

            Creature* creature = (Creature*)m_targets.getUnitTarget();
            if ( creature->GetCreatureType() != CREATURE_TYPE_CRITTER && ( !creature->lootForBody || creature->lootForSkin || !creature->loot.empty() ) )
            {
                return SPELL_FAILED_TARGET_NOT_LOOTED;
            }

            uint32 skill = creature->GetCreatureInfo()->GetRequiredLootSkill();

            int32 skillValue = ((Player*)m_caster)->GetSkillValue(skill);
            int32 TargetLevel = m_targets.getUnitTarget()->getLevel();
            int32 ReqValue = (skillValue < 100 ? (TargetLevel-10) * 10 : TargetLevel * 5);
            if (ReqValue > skillValue)
                return SPELL_FAILED_SKILL_NOT_HIGH_ENOUGH;

            // chance for fail at orange skinning attempt
            if( (m_selfContainer && (*m_selfContainer) == this) &&
                    skillValue < sWorld.GetConfigMaxSkillValue() &&
                    (ReqValue < 0 ? 0 : ReqValue) > irand(skillValue - 25, skillValue + 37) )
                return SPELL_FAILED_TRY_AGAIN;

            break;
        }
        case SPELL_EFFECT_OPEN_LOCK_ITEM:
        case SPELL_EFFECT_OPEN_LOCK:
        {
            if (m_caster->GetTypeId() != TYPEID_PLAYER)  // only players can open locks, gather etc.
                return SPELL_FAILED_BAD_TARGETS;

            // we need a go target in case of TARGET_GAMEOBJECT (for other targets acceptable GO and items)
            if (m_spellInfo->EffectImplicitTargetA[i] == TARGET_GAMEOBJECT)
            {
                if (!m_targets.getGOTarget())
                    return SPELL_FAILED_BAD_TARGETS;

    
                if(GameObject *pGo = m_targets.getGOTarget())
                {
                    if(WorldObject *owner = pGo->GetOwner())
                    {
                        if(pGo && pGo->GetOwner()->GetTypeId() == TYPEID_PLAYER && pGo->IsFriendlyTo(m_caster))
                            return SPELL_FAILED_BAD_TARGETS;
                    }
                }
            }

            // get the lock entry
            uint32 lockId = 0;
            if (GameObject* go = m_targets.getGOTarget())
            {
                // Hunter traps can be unlocked in BG's - other than that all the
                // game objects have special rules that the players have to follow to use
                // them
                if(  go->GetGoType() != GAMEOBJECT_TYPE_TRAP &&
                        ((Player*)m_caster)->InBattleGround() &&
                        !((Player*)m_caster)->CanUseBattleGroundObject() )
                    return SPELL_FAILED_TRY_AGAIN;

                lockId = go->GetGOInfo()->GetLockId();
                if (!lockId)
                    return SPELL_FAILED_ALREADY_OPEN;
            }
            else if(Item* item = m_targets.getItemTarget())
            {
                // not own (trade?)
                if (item->GetOwner() != m_caster)
                    return SPELL_FAILED_ITEM_GONE;

                lockId = item->GetProto()->LockID;

                // if already unlocked
                if (!lockId || item->HasFlag(ITEM_FIELD_FLAGS, ITEM_DYNFLAG_UNLOCKED))
                    return SPELL_FAILED_ALREADY_OPEN;
            }
            else
                return SPELL_FAILED_BAD_TARGETS;

            SkillType skillId = SKILL_NONE;
            int32 reqSkillValue = 0;
            int32 skillValue = 0;

            // check lock compatibility
            SpellCastResult res = CanOpenLock(SpellEffectIndex(i), lockId, skillId, reqSkillValue, skillValue);
            if(res != SPELL_CAST_OK)
                return res;

            // chance for fail at orange mining/herb/LockPicking gathering attempt
            // second check prevent fail at rechecks
            if(skillId != SKILL_NONE && (!m_selfContainer || ((*m_selfContainer) != this)))
            {
                bool canFailAtMax = skillId != SKILL_HERBALISM && skillId != SKILL_MINING;

                // chance for failure in orange gather / lockpick (gathering skill can't fail at maxskill)
                if((canFailAtMax || skillValue < sWorld.GetConfigMaxSkillValue()) && reqSkillValue > irand(skillValue - 25, skillValue + 37))
                    return SPELL_FAILED_TRY_AGAIN;
            }
            break;
        }
        case SPELL_EFFECT_SUMMON_DEAD_PET:
        {
            Creature *pet = m_caster->GetPet();
            if(!pet)
            {
                Player* pPlayer = dynamic_cast<Player*>(m_caster);
                // We do allow Revive Pet if the pet is despawned.
                if (pPlayer && m_spellInfo->Id == 982)
                {
                    if (!Pet::IsPetDeadInDB(pPlayer))
                    {
                        return SPELL_FAILED_ALREADY_HAVE_SUMMON;
                    }
                    else
                        break;
                }

                return SPELL_FAILED_NO_PET;
            }

            if(pet->isAlive())
                return SPELL_FAILED_ALREADY_HAVE_SUMMON;

            break;
        }
        // Don't make this check for SPELL_EFFECT_SUMMON_CRITTER, SPELL_EFFECT_SUMMON_WILD or SPELL_EFFECT_SUMMON_GUARDIAN.
        // These won't show up in m_caster->GetPetGUID()
        case SPELL_EFFECT_SUMMON:
        case SPELL_EFFECT_SUMMON_POSSESSED:
            if(m_spellInfo->Id == 126)
                break;
        case SPELL_EFFECT_SUMMON_PHANTASM:
        case SPELL_EFFECT_SUMMON_DEMON:
        {
            if (m_caster->GetPetGuid())
                return SPELL_FAILED_ALREADY_HAVE_SUMMON;

            if (m_caster->GetCharmGuid())
                return SPELL_FAILED_ALREADY_HAVE_CHARM;

            break;
        }
        case SPELL_EFFECT_SUMMON_PET:
        {
            if (m_caster->GetPetGuid())                 // let warlock do a replacement summon
            {

                Pet* pet = ((Player*)m_caster)->GetPet();

                if (m_caster->GetTypeId() == TYPEID_PLAYER && m_caster->getClass() == CLASS_WARLOCK)
                {
                    if (strict)                         //Summoning Disorientation, trigger pet stun (cast by pet so it doesn't attack player)
                        pet->CastSpell(pet, 32752, true, NULL, NULL, pet->GetObjectGuid());
                }
                else
                    return SPELL_FAILED_ALREADY_HAVE_SUMMON;
            }

            if (m_caster->GetCharmGuid())
                return SPELL_FAILED_ALREADY_HAVE_CHARM;

            break;
        }
        case SPELL_EFFECT_SUMMON_PLAYER:
        {
            if (m_caster->GetTypeId() != TYPEID_PLAYER)
                return SPELL_FAILED_BAD_TARGETS;
            if (!((Player*)m_caster)->GetSelectionGuid())
                return SPELL_FAILED_BAD_TARGETS;

            Player* target = sObjectMgr.GetPlayer(((Player*)m_caster)->GetSelectionGuid());
            if (!target || ((Player*)m_caster) == target || !target->IsInSameRaidWith((Player*)m_caster))
                return SPELL_FAILED_BAD_TARGETS;

            // check if our map is dungeon
            if( m_caster->GetMap() && !m_caster->GetMap()->GetPlayer(target->GetObjectGuid()) && (m_caster->GetMapId() > 1 || target->GetMapId() > 1) )
                return SPELL_FAILED_TARGET_NOT_IN_INSTANCE;
            break;
        }
        case SPELL_EFFECT_LEAP:
        {
            // not allow use this effect at battleground until battleground start
            if(m_caster->GetTypeId() == TYPEID_PLAYER)
                if(BattleGround const *bg = ((Player*)m_caster)->GetBattleGround())
                    if(bg->GetStatus() != STATUS_IN_PROGRESS)
                        return SPELL_FAILED_TRY_AGAIN;
            break;
        }
        case SPELL_EFFECT_TELEPORT_UNITS_FACE_CASTER:
        {
            float dis = GetSpellRadius(sSpellRadiusStore.LookupEntry(m_spellInfo->EffectRadiusIndex[i]));
            float fx = m_caster->GetPositionX() + dis * cos(m_caster->GetOrientation());
            float fy = m_caster->GetPositionY() + dis * sin(m_caster->GetOrientation());
            // teleport a bit above terrain level to avoid falling below it
            float fz = m_caster->GetTerrain()->GetHeight(fx, fy, m_caster->GetPositionZ(), true);
            if(fz <= INVALID_HEIGHT)                    // note: this also will prevent use effect in instances without vmaps height enabled
                return SPELL_FAILED_TRY_AGAIN;

            float caster_pos_z = m_caster->GetPositionZ();
            // Control the caster to not climb or drop when +-fz > 8
            if(!(fz <= caster_pos_z + 8 && fz >= caster_pos_z - 8))
                return SPELL_FAILED_TRY_AGAIN;

            // not allow use this effect at battleground until battleground start
            if(m_caster->GetTypeId() == TYPEID_PLAYER)
                if(BattleGround const *bg = ((Player*)m_caster)->GetBattleGround())
                    if(bg->GetStatus() != STATUS_IN_PROGRESS)
                        return SPELL_FAILED_TRY_AGAIN;
            break;
        }
        default:
            break;
        }
    }

    for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        switch(m_spellInfo->EffectApplyAuraName[i])
        {
        case SPELL_AURA_MOD_POSSESS:
        {
            if (m_caster->GetTypeId() != TYPEID_PLAYER)
                return SPELL_FAILED_UNKNOWN;

            if (m_targets.getUnitTarget() == m_caster)
                return SPELL_FAILED_BAD_TARGETS;

            if (m_caster->GetPetGuid())
                return SPELL_FAILED_ALREADY_HAVE_SUMMON;

            if (m_caster->GetCharmGuid())
                return SPELL_FAILED_ALREADY_HAVE_CHARM;

            if (m_caster->GetCharmerGuid())
                return SPELL_FAILED_CHARMED;

            if (!m_targets.getUnitTarget())
                return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

            if (m_targets.getUnitTarget()->GetCharmerGuid())
                return SPELL_FAILED_CHARMED;

            if (int32(m_targets.getUnitTarget()->getLevel()) > CalculateDamage(SpellEffectIndex(i),m_targets.getUnitTarget()))
                return SPELL_FAILED_HIGHLEVEL;

            break;
        }
        case SPELL_AURA_MOD_CHARM:
        {
            if (m_targets.getUnitTarget() == m_caster)
                return SPELL_FAILED_BAD_TARGETS;

            if (m_caster->GetPetGuid())
                return SPELL_FAILED_ALREADY_HAVE_SUMMON;

            if (m_caster->GetCharmGuid() && m_spellInfo->Id != 23174) // Chromatic Mutation: Chromaggus in BWL should be able to MC several targets.
                return SPELL_FAILED_ALREADY_HAVE_CHARM;

            if (m_caster->GetCharmerGuid())
                return SPELL_FAILED_CHARMED;

            if (!m_targets.getUnitTarget())
                return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

            if (m_targets.getUnitTarget()->GetCharmerGuid())
                return SPELL_FAILED_CHARMED;

            if (int32(m_targets.getUnitTarget()->getLevel()) > CalculateDamage(SpellEffectIndex(i),m_targets.getUnitTarget()))
                return SPELL_FAILED_HIGHLEVEL;

            break;
        }
        case SPELL_AURA_MOD_POSSESS_PET:
        {
            if (m_caster->GetTypeId() != TYPEID_PLAYER)
                return SPELL_FAILED_UNKNOWN;

            if (m_caster->GetCharmGuid())
                return SPELL_FAILED_ALREADY_HAVE_CHARM;

            if (m_caster->GetCharmerGuid())
                return SPELL_FAILED_CHARMED;

            Pet* pet = m_caster->GetPet();
            if (!pet)
                return SPELL_FAILED_NO_PET;

            if (pet->GetCharmerGuid())
                return SPELL_FAILED_CHARMED;

            break;
        }
        case SPELL_AURA_MOUNTED:
        {
            if (m_caster->IsInWater())
                return SPELL_FAILED_ONLY_ABOVEWATER;

            if (m_caster->GetTypeId() == TYPEID_PLAYER && ((Player*)m_caster)->GetTransport())
                return SPELL_FAILED_NO_MOUNTS_ALLOWED;

            /// Specific case for Temple of Ahn'Qiraj mounts as they are usable only in AQ40 and are the only mounts allowed here
            /// TBC and above handle this by using m_spellInfo->AreaId
            bool isAQ40Mounted = false;

            switch (m_spellInfo->Id)
            {
            case 25863:    // spell used by ingame item for Black Qiraji mount (legendary reward)
            case 26655:    // spells also related to Black Qiraji mount but use/trigger unknown
            case 26656:
            case 31700:
                if (m_caster->GetMapId() == 531)
                    isAQ40Mounted = true;
                break;
            case 25953:    // spells of the 4 regular AQ40 mounts
            case 26054:
            case 26055:
            case 26056:
                if (m_caster->GetMapId() == 531)
                {
                    isAQ40Mounted = true;
                    break;
                }
                else
                    return SPELL_FAILED_NOT_HERE;
            default:
                break;
            }

            // Ignore map check if spell have AreaId. AreaId already checked and this prevent special mount spells
            if (!isAQ40Mounted && m_caster->GetTypeId() == TYPEID_PLAYER && !sMapStore.LookupEntry(m_caster->GetMapId())->IsMountAllowed() && !m_IsTriggeredSpell) //[-ZERO] && !m_spellInfo->AreaId)
                return SPELL_FAILED_NO_MOUNTS_ALLOWED;

            if (m_caster->GetAreaId()==35)
                return SPELL_FAILED_NO_MOUNTS_ALLOWED;

            if (m_caster->IsInDisallowedMountForm())
                return SPELL_FAILED_NOT_SHAPESHIFT;

            break;
        }
        case SPELL_AURA_RANGED_ATTACK_POWER_ATTACKER_BONUS:
        {
            if(!m_targets.getUnitTarget())
                return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

            // can be casted at non-friendly unit or own pet/charm
            if(m_caster->IsFriendlyTo(m_targets.getUnitTarget()))
                return SPELL_FAILED_TARGET_FRIENDLY;

            break;
        }
        case SPELL_AURA_PERIODIC_MANA_LEECH:
        {
            if (!m_targets.getUnitTarget())
                return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

            if (m_caster->GetTypeId() != TYPEID_PLAYER || m_CastItem)
                break;

            if(m_targets.getUnitTarget()->getPowerType() != POWER_MANA)
                return SPELL_FAILED_BAD_TARGETS;

            break;
        }
        default:
            break;
        }
    }

    // check trade slot case (last, for allow catch any another cast problems)
    if (m_targets.m_targetMask & TARGET_FLAG_TRADE_ITEM)
    {
        if (m_caster->GetTypeId() != TYPEID_PLAYER)
            return SPELL_FAILED_NOT_TRADING;

        Player *pCaster = ((Player*)m_caster);
        TradeData* my_trade = pCaster->GetTradeData();

        if (!my_trade)
            return SPELL_FAILED_NOT_TRADING;

        TradeSlots slot = TradeSlots(m_targets.getItemTargetGuid().GetRawValue());
        if (slot != TRADE_SLOT_NONTRADED)
            return SPELL_FAILED_ITEM_NOT_READY;

        // if trade not complete then remember it in trade data
        if (!my_trade->IsInAcceptProcess())
        {
            // Spell will be casted at completing the trade. Silently ignore at this place
            my_trade->SetSpell(m_spellInfo->Id, m_CastItem);
            return SPELL_FAILED_DONT_REPORT;
        }
    }

    // all ok
    return SPELL_CAST_OK;
}

SpellCastResult Spell::CheckPetCast(Unit* target)
{
    if(!m_caster->isAlive())
        return SPELL_FAILED_CASTER_DEAD;

    if(m_caster->IsNonMeleeSpellCasted(false))              //prevent spellcast interruption by another spellcast
        return SPELL_FAILED_SPELL_IN_PROGRESS;
    if(m_caster->isInCombat() && IsNonCombatSpell(m_spellInfo))
        return SPELL_FAILED_AFFECTING_COMBAT;

    if(m_caster->GetTypeId()==TYPEID_UNIT && (((Creature*)m_caster)->IsPet() || m_caster->isCharmed()))
    {
        //dead owner (pets still alive when owners ressed?)
        if(m_caster->GetCharmerOrOwner() && !m_caster->GetCharmerOrOwner()->isAlive())
            return SPELL_FAILED_CASTER_DEAD;

        if(!target && m_targets.getUnitTarget())
            target = m_targets.getUnitTarget();

        bool need = false;
        for(int i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            if (m_spellInfo->EffectImplicitTargetA[i] == TARGET_CHAIN_DAMAGE ||
                    m_spellInfo->EffectImplicitTargetA[i] == TARGET_SINGLE_FRIEND ||
                    m_spellInfo->EffectImplicitTargetA[i] == TARGET_SINGLE_FRIEND_2 ||
                    m_spellInfo->EffectImplicitTargetA[i] == TARGET_DUELVSPLAYER ||
                    m_spellInfo->EffectImplicitTargetA[i] == TARGET_SINGLE_PARTY ||
                    m_spellInfo->EffectImplicitTargetA[i] == TARGET_CURRENT_ENEMY_COORDINATES)
            {
                need = true;
                if(!target)
                    return SPELL_FAILED_BAD_IMPLICIT_TARGETS;
                break;
            }
        }
        if(need)
            m_targets.setUnitTarget(target);

        Unit* _target = m_targets.getUnitTarget();

        if(_target)                                         //for target dead/target not valid
        {
            if (!_target->isTargetableForAttack())
                return SPELL_FAILED_BAD_TARGETS;            // guessed error

            if( m_spellInfo->SpellIconID != 47 && IsPositiveSpell(m_spellInfo->Id)) // Spell Icon ID 47 is for the Felhunter Devour magic.
            {
                if(m_caster->IsHostileTo(_target))
                    return SPELL_FAILED_BAD_TARGETS;
            }
            else
            {
                bool duelvsplayertar = false;
                for(int j = 0; j < MAX_EFFECT_INDEX; ++j)
                {
                    //TARGET_DUELVSPLAYER is positive AND negative
                    duelvsplayertar |= (m_spellInfo->EffectImplicitTargetA[j] == TARGET_DUELVSPLAYER);
                }
                if(m_caster->IsFriendlyTo(target) && !duelvsplayertar)
                {
                    return SPELL_FAILED_BAD_TARGETS;
                }
            }
        }
        //cooldown
        if(((Creature*)m_caster)->HasSpellCooldown(m_spellInfo->Id))
            return SPELL_FAILED_NOT_READY;
    }

    return CheckCast(true);
}

SpellCastResult Spell::CheckCasterAuras() const
{
    // Flag drop spells totally immuned to caster auras
    // FIXME: find more nice check for all totally immuned spells
    // AttributesEx3 & 0x10000000?
    if (m_spellInfo->Id == 23336 ||                         // Alliance Flag Drop
            m_spellInfo->Id == 23334)                           // Horde Flag Drop
        return SPELL_CAST_OK;

    uint8 school_immune = 0;
    uint32 mechanic_immune = 0;
    uint32 dispel_immune = 0;

    // Check if the spell grants school or mechanic immunity.
    // We use bitmasks so the loop is done only once and not on every aura check below.
    if ( m_spellInfo->AttributesEx & SPELL_ATTR_EX_DISPEL_AURAS_ON_IMMUNITY )
    {
        for(int i = 0; i < MAX_EFFECT_INDEX; ++i)
        {
            if (m_spellInfo->EffectApplyAuraName[i] == SPELL_AURA_SCHOOL_IMMUNITY)
                school_immune |= uint32(m_spellInfo->EffectMiscValue[i]);
            else if (m_spellInfo->EffectApplyAuraName[i] == SPELL_AURA_MECHANIC_IMMUNITY)
                mechanic_immune |= 1 << uint32(m_spellInfo->EffectMiscValue[i]-1);
            else if (m_spellInfo->EffectApplyAuraName[i] == SPELL_AURA_MECHANIC_IMMUNITY_MASK)
                mechanic_immune |= uint32(m_spellInfo->EffectMiscValue[i]);
            else if (m_spellInfo->EffectApplyAuraName[i] == SPELL_AURA_DISPEL_IMMUNITY)
                dispel_immune |= GetDispellMask(DispelType(m_spellInfo->EffectMiscValue[i]));
        }
    }

    // Check whether the cast should be prevented by any state you might have.
    SpellCastResult prevented_reason = SPELL_CAST_OK;
    // Have to check if there is a stun aura. Otherwise will have problems with ghost aura apply while logging out
    uint32 unitflag = m_caster->GetUInt32Value(UNIT_FIELD_FLAGS);     // Get unit state
    /*[-ZERO]    if (unitflag & UNIT_FLAG_STUNNED && !(m_spellInfo->AttributesEx5 & SPELL_ATTR_EX5_USABLE_WHILE_STUNNED))
            prevented_reason = SPELL_FAILED_STUNNED;
        else if (unitflag & UNIT_FLAG_CONFUSED && !(m_spellInfo->AttributesEx5 & SPELL_ATTR_EX5_USABLE_WHILE_CONFUSED))
            prevented_reason = SPELL_FAILED_CONFUSED;
        else if (unitflag & UNIT_FLAG_FLEEING && !(m_spellInfo->AttributesEx5 & SPELL_ATTR_EX5_USABLE_WHILE_FEARED))
            prevented_reason = SPELL_FAILED_FLEEING;
        else */
    if (unitflag & UNIT_FLAG_SILENCED && m_spellInfo->PreventionType == SPELL_PREVENTION_TYPE_SILENCE)
        prevented_reason = SPELL_FAILED_SILENCED;
    else if (unitflag & UNIT_FLAG_PACIFIED && m_spellInfo->PreventionType == SPELL_PREVENTION_TYPE_PACIFY)
        prevented_reason = SPELL_FAILED_PACIFIED;

    // Attr must make flag drop spell totally immune from all effects
    if (prevented_reason != SPELL_CAST_OK)
    {
        if (school_immune || mechanic_immune || dispel_immune)
        {
            //Checking auras is needed now, because you are prevented by some state but the spell grants immunity.
            Unit::SpellAuraHolderMap const& auras = m_caster->GetSpellAuraHolderMap();
            for(Unit::SpellAuraHolderMap::const_iterator itr = auras.begin(); itr != auras.end(); ++itr)
            {
                SpellAuraHolder *holder = itr->second;
                SpellEntry const * pEntry = holder->GetSpellProto();

                if ((GetSpellSchoolMask(pEntry) & school_immune) && !(pEntry->AttributesEx & SPELL_ATTR_EX_UNAFFECTED_BY_SCHOOL_IMMUNE))
                    continue;
                if ((1<<(pEntry->Dispel)) & dispel_immune)
                    continue;

                for (int32 i = 0; i < MAX_EFFECT_INDEX; ++i)
                {
                    Aura *aura = holder->GetAuraByEffectIndex(SpellEffectIndex(i));
                    if (!aura)
                        continue;

                    if (GetSpellMechanicMask(pEntry, 1 << i) & mechanic_immune)
                        continue;
                    // Make a second check for spell failed so the right SPELL_FAILED message is returned.
                    // That is needed when your casting is prevented by multiple states and you are only immune to some of them.
                    switch(aura->GetModifier()->m_auraname)
                    {
                        /* Zero
                        case SPELL_AURA_MOD_STUN:
                            if (!(m_spellInfo->AttributesEx5 & SPELL_ATTR_EX5_USABLE_WHILE_STUNNED))
                                return SPELL_FAILED_STUNNED;
                            break;
                        case SPELL_AURA_MOD_CONFUSE:
                            if (!(m_spellInfo->AttributesEx5 & SPELL_ATTR_EX5_USABLE_WHILE_CONFUSED))
                                return SPELL_FAILED_CONFUSED;
                            break;
                        case SPELL_AURA_MOD_FEAR:
                            if (!(m_spellInfo->AttributesEx5 & SPELL_ATTR_EX5_USABLE_WHILE_FEARED))
                                return SPELL_FAILED_FLEEING;
                            break;
                        */
                    case SPELL_AURA_MOD_SILENCE:
                    case SPELL_AURA_MOD_PACIFY:
                    case SPELL_AURA_MOD_PACIFY_SILENCE:
                        if( m_spellInfo->PreventionType == SPELL_PREVENTION_TYPE_PACIFY)
                            return SPELL_FAILED_PACIFIED;
                        else if ( m_spellInfo->PreventionType == SPELL_PREVENTION_TYPE_SILENCE)
                            return SPELL_FAILED_SILENCED;
                        break;
                    default:
                        break;
                    }
                }
            }
        }
        // You are prevented from casting and the spell casted does not grant immunity. Return a failed error.
        else
            return prevented_reason;
    }
    return SPELL_CAST_OK;
}

bool Spell::CheckPaladinBlessingStacking(SpellEntry const* spellProto)
{
    // Handling to keep paladin greater and lesser blessings from stacking.
    std::list<std::pair<Unit*, SpellEntry const*> > auraRemoveList;

    if (!spellProto)
        return true;

    for(TargetList::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        TargetInfo currentTargetInfo = *ihit;
        Unit* currentTarget = m_caster->GetMap()->GetUnit(currentTargetInfo.targetGUID);
        Unit::AuraList const& blessingList = currentTarget->GetAurasByType(AuraType(spellProto->EffectApplyAuraName[0]));

        if (currentTarget && spellProto->SpellFamilyName == SPELLFAMILY_PALADIN && strstr(spellProto->SpellName[0], "Blessing") != nullptr)
        {
            bool castIsGreater = strstr(spellProto->SpellName[0], "Greater") != nullptr ? true : false;

            std::string castSpellName = spellProto->SpellName[0];
            std::string greaterCastSpellName = "Greater " + castSpellName;

            SpellEntry const* removeAuraSpell = nullptr;

            for (Aura* blessingAura : blessingList)
            {
                if (!blessingAura || !blessingAura->IsPositive())
                    continue;

                SpellEntry const* blessingAuraSpell = blessingAura->GetSpellProto();

                if (!castIsGreater && greaterCastSpellName.compare(blessingAuraSpell->SpellName[0]) == 0 && currentTarget == unitTarget) // Check if the aura is cast by a greater blessing. In that case a lesser blessing should not be castable.
                {
                    return false;
                }
                else if (castIsGreater && castSpellName.find(blessingAuraSpell->SpellName[0]) != std::string::npos) // If a greater blessing is cast all lesser ones should be removed.
                {
                    removeAuraSpell = blessingAuraSpell;
                }
            }

            if (removeAuraSpell)
            {
                auraRemoveList.push_back(std::make_pair(currentTarget, removeAuraSpell));
            }
        }
    }

    for (std::pair<Unit*, SpellEntry const*> currentPair : auraRemoveList)
    {
        currentPair.first->RemoveAurasDueToSpell(currentPair.second->Id);
    }

    return true;
}

bool Spell::CheckHOTStacking(SpellEntry const* spellProto)
{
    if (spellProto && spellProto->EffectApplyAuraName[0] == 8 && unitTarget)
    {
        Unit::AuraList const& healingList = unitTarget->GetAurasByType(AuraType(spellProto->EffectApplyAuraName[0]));

        uint32 m_healingValue = m_caster->CalculateSpellDamage(unitTarget, spellProto,(SpellEffectIndex) 0, &m_currentBasePoints[0]);
        m_healingValue = m_caster->SpellHealingBonusDone(unitTarget, spellProto, m_healingValue, DOT, 1);

        for (Aura* healingAura : healingList)
        {
            Modifier* m_healingModifier = healingAura->GetModifier();
            SpellEntry const* healingSpellproto = healingAura->GetSpellProto();

            int spellNameComparison = strcmp((const char*) healingSpellproto->SpellName, (const char*) spellProto->SpellName);

            if ((uint32) m_healingModifier->m_amount > m_healingValue && spellNameComparison == 0 ) // If a currently existing HoT is stronger than the attempted casted one we do not apply the new HoT.
            {
                return false;
            }
            else if ((uint32) m_healingModifier->m_amount <= m_healingValue && spellNameComparison == 0) // If the currently existing HoT is weaker than the attempted one we replace the current HoT.
            {
                unitTarget->RemoveAurasDueToSpell(healingAura->GetId());
                return true;
            }
        }
    }

    return true;
}

bool Spell::CheckBuffOverwrite(SpellEntry const* spellProto)
{
    if (!spellProto)
        return true;

    // Hidden Buffs should not be checked, such as those from gear.
    if (spellProto->Attributes & SPELL_ATTR_HIDDEN)
       return true;

    // Buffs are positive spells.
    if (spellProto->AttributesEx & SPELL_ATTR_EX_NEGATIVE)
       return true;

    for (short i = 0; i < MAX_EFFECT_INDEX; i++)
    {
        const AuraType auraTypes[2] = { SPELL_AURA_MOD_STAT, SPELL_AURA_MOD_RESISTANCE_EXCLUSIVE };
        for (short y = 0; y < 2; y++)
        {
            if (spellProto->EffectApplyAuraName[i] == auraTypes[y])
            {
                for(TargetList::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
                {
                    Unit* pTarget = m_caster->GetMap()->GetUnit(ihit->targetGUID);

                    if (!pTarget)
                        continue;

                    Unit::AuraList const& list = pTarget->GetAurasByType(auraTypes[y]);

                    for (Aura* aura : list)
                    {

                        // If the buff is supposed to stack we ignore it.
                        if (!sSpellMgr.IsNoStackSpellDueToSpell(aura->GetId(), spellProto->Id))
                            continue;

                        if (!IsNoStackAuraDueToAura(aura->GetId(), spellProto->Id))
                            continue;

                        Modifier* pMod = aura->GetModifier();

                        // Make sure that the buffs affect the same stats.
                        if (pMod->m_miscvalue == spellProto->EffectMiscValue[i])
                        {
                            // If the new buff has a shorter duration we don't allow overwriting.
                            if (aura->GetAuraDuration() > CalculateSpellDuration(spellProto, m_caster))
                                return false; 


                            if (pTarget)
                            {
                                int32 m_currentBasePoints = spellProto->CalculateSimpleValue(SpellEffectIndex(i));

                                int32 effect = m_caster->CalculateSpellDamage(pTarget, spellProto, SpellEffectIndex(i), &m_currentBasePoints);

                                // If the already applied spell is more powerful we do
                                // not allow it to be overwritten.
                                if (abs(pMod->m_amount) > abs(effect))
                                    return false;
                            }
                        }
                    }
                }
            }
        }
    }

    return true;
}

bool Spell::CanAutoCast(Unit* target)
{
    ObjectGuid targetguid = target->GetObjectGuid();

    for(int j = 0; j < MAX_EFFECT_INDEX; ++j)
    {
        if(m_spellInfo->Effect[j] == SPELL_EFFECT_APPLY_AURA)
        {
            if( m_spellInfo->StackAmount <= 1)
            {
                if( target->HasAura(m_spellInfo->Id, SpellEffectIndex(j)) )
                    return false;
            }
            else
            {
                if(Aura* aura = target->GetAura(m_spellInfo->Id, SpellEffectIndex(j)))
                    if(aura->GetStackAmount() >= m_spellInfo->StackAmount)
                        return false;
            }
        }
        else if ( IsAreaAuraEffect( m_spellInfo->Effect[j] ))
        {
            if( target->HasAura(m_spellInfo->Id, SpellEffectIndex(j)) )
                return false;
        }
    }

    SpellCastResult result = CheckPetCast(target);

    if(result == SPELL_CAST_OK || result == SPELL_FAILED_UNIT_NOT_INFRONT)
    {
        FillTargetMap();
        //check if among target units, our WANTED target is as well (->only self cast spells return false)
        for(TargetList::const_iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
            if (ihit->targetGUID == targetguid)
                return true;
    }
    return false;                                           //target invalid
}

SpellCastResult Spell::CheckRange(bool strict)
{
    Unit *target = m_targets.getUnitTarget();

    // special range cases
    switch(m_spellInfo->rangeIndex)
    {
        // self cast doesn't need range checking -- also for Starshards fix
    case SPELL_RANGE_IDX_SELF_ONLY:
        return SPELL_CAST_OK;
        // combat range spells are treated differently
    case SPELL_RANGE_IDX_COMBAT:
    {
        if (target)
        {
            if (target == m_caster)
                return SPELL_CAST_OK;

            float range_mod = strict ? 0.0f : 5.0f;
            float base = ATTACK_DISTANCE;
            if (Player* modOwner = m_caster->GetSpellModOwner())
                range_mod += modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_RANGE, base, this);

            // with additional 5 dist for non stricted case (some melee spells have delay in apply
            return m_caster->CanReachWithMeleeAttack(target, range_mod) ? SPELL_CAST_OK : SPELL_FAILED_OUT_OF_RANGE;
        }
        break;                                          // let continue in generic way for no target
    }
    }

    //add radius of caster and ~5 yds "give" for non stricred (landing) check
    float range_mod = strict ? 1.25f : 6.25;

    SpellRangeEntry const* srange = sSpellRangeStore.LookupEntry(m_spellInfo->rangeIndex);
    float max_range = GetSpellMaxRange(srange) + range_mod;
    float min_range = GetSpellMinRange(srange);

    if(Player* modOwner = m_caster->GetSpellModOwner())
        modOwner->ApplySpellMod(m_spellInfo->Id, SPELLMOD_RANGE, max_range, this);

    if(target && target != m_caster)
    {
        // distance from target in checks
        float dist = m_caster->GetCombatDistance(target);

        if(dist > max_range)
            return SPELL_FAILED_OUT_OF_RANGE;
        if(min_range && dist < min_range)
            return SPELL_FAILED_TOO_CLOSE;
        if( m_caster->GetTypeId() == TYPEID_PLAYER &&
                ( sSpellMgr.GetSpellFacingFlag(m_spellInfo->Id) & SPELL_FACING_FLAG_INFRONT ) && !m_caster->HasInArc( M_PI_F, target ) )
            return SPELL_FAILED_UNIT_NOT_INFRONT;
    }

    //Range check for spells that target game objects
    if (!target && m_targets.getGOTarget())
    {
        float dist = m_caster->GetDistance(m_targets.getGOTarget());
        if (dist > max_range)
            return SPELL_FAILED_OUT_OF_RANGE;
    }

    // TODO verify that such spells really use bounding radius
    if(m_targets.m_targetMask == TARGET_FLAG_DEST_LOCATION && m_targets.m_destX != 0 && m_targets.m_destY != 0 && m_targets.m_destZ != 0)
    {
        if(!m_caster->IsWithinDist3d(m_targets.m_destX, m_targets.m_destY, m_targets.m_destZ, max_range))
            return SPELL_FAILED_OUT_OF_RANGE;
        if(min_range && m_caster->IsWithinDist3d(m_targets.m_destX, m_targets.m_destY, m_targets.m_destZ, min_range))
            return SPELL_FAILED_TOO_CLOSE;
    }

    if((m_spellInfo->Id == 11578 || m_spellInfo->Id == 100 || m_spellInfo->Id == 6178 || m_spellInfo->Id == 20252 || 
        m_spellInfo->Id == 20616 || m_spellInfo->Id == 20617 || m_spellInfo->Id == 16979 || m_spellInfo->Id == 22641 ) && target && !target->IsInWater())
    {
        PathInfo path(m_caster, target->GetPositionX(), target->GetPositionY(), target->GetPositionZ());
        PointPath pointPath = path.getFullPath();
        if(pointPath.GetTotalLength() > max_range + 10.0f)
        {
            return SPELL_FAILED_NOPATH;
        }

        MMAP::MMapManager* mmap = MMAP::MMapFactory::createOrGetMMapManager();
        dtPolyRef polyRef;

        float tempx, tempy, temz;
        target->GetPosition(tempx,tempy,temz);

        if (!mmap->GetNearestValidPosition(target,3,3,5,tempx,tempy,temz,&polyRef))
            return SPELL_FAILED_NOPATH;
    }

    return SPELL_CAST_OK;
}

uint32 Spell::CalculatePowerCost(SpellEntry const* spellInfo, Unit* caster, Spell const* spell, Item* castItem)
{
    // item cast not used power
    if (castItem)
        return 0;

    // Spell drain all exist power on cast (Only paladin lay of Hands)
    if (spellInfo->AttributesEx & SPELL_ATTR_EX_DRAIN_ALL_POWER)
    {
        // If power type - health drain all
        if (spellInfo->powerType == POWER_HEALTH)
            return caster->GetHealth();
        // Else drain all power
        if (spellInfo->powerType < MAX_POWERS)
            return caster->GetPower(Powers(spellInfo->powerType));
        sLog.outError("Spell::CalculateManaCost: Unknown power type '%d' in spell %d", spellInfo->powerType, spellInfo->Id);
        return 0;
    }

    // Base powerCost
    int32 powerCost = spellInfo->manaCost;
    // PCT cost from total amount
    if (spellInfo->ManaCostPercentage)
    {
        switch (spellInfo->powerType)
        {
            // health as power used
        case POWER_HEALTH:
            powerCost += spellInfo->ManaCostPercentage * caster->GetCreateHealth() / 100;
            break;
        case POWER_MANA:
            powerCost += spellInfo->ManaCostPercentage * caster->GetCreateMana() / 100;
            break;
        case POWER_RAGE:
        case POWER_FOCUS:
        case POWER_ENERGY:
        case POWER_HAPPINESS:
            powerCost += spellInfo->ManaCostPercentage * caster->GetMaxPower(Powers(spellInfo->powerType)) / 100;
            break;
        default:
            sLog.outError("Spell::CalculateManaCost: Unknown power type '%d' in spell %d", spellInfo->powerType, spellInfo->Id);
            return 0;
        }
    }
    SpellSchools school = GetFirstSchoolInMask(spell ? spell->m_spellSchoolMask : GetSpellSchoolMask(spellInfo));
    // Flat mod from caster auras by spell school
    powerCost += caster->GetInt32Value(UNIT_FIELD_POWER_COST_MODIFIER + school);
    // Shiv - costs 20 + weaponSpeed*10 energy (apply only to non-triggered spell with energy cost)
    if (spellInfo->AttributesEx4 & SPELL_ATTR_EX4_SPELL_VS_EXTEND_COST)
        powerCost += caster->GetAttackTime(OFF_ATTACK) / 100;
    // Apply cost mod by spell
    if (spell && (spellInfo->manaCost != 0 || spellInfo->ManaCostPercentage != 0 || spellInfo->manaCostPerlevel != 0))  // Spells that cost no mana should not use auras that alter power cost.
        if (Player* modOwner = caster->GetSpellModOwner())
            modOwner->ApplySpellMod(spellInfo->Id, SPELLMOD_COST, powerCost, spell);

    if (spellInfo->Attributes & SPELL_ATTR_LEVEL_DAMAGE_CALCULATION)
        powerCost = int32(powerCost/ (1.117f * spellInfo->spellLevel / caster->getLevel() -0.1327f));

    // PCT mod from user auras by school
    powerCost = int32(powerCost * (1.0f + caster->GetFloatValue(UNIT_FIELD_POWER_COST_MULTIPLIER + school)));
    if (powerCost < 0)
        powerCost = 0;
    return powerCost;
}

SpellCastResult Spell::CheckPower()
{
    // item cast not used power
    if(m_CastItem)
        return SPELL_CAST_OK;

    // health as power used - need check health amount
    if (m_spellInfo->powerType == POWER_HEALTH)
    {
        if (m_caster->GetHealth() <= m_powerCost)
            return SPELL_FAILED_CANT_DO_THAT_YET;
        return SPELL_CAST_OK;
    }

    // Check valid power type
    if (m_spellInfo->powerType >= MAX_POWERS)
    {
        sLog.outError("Spell::CheckMana: Unknown power type '%d'", m_spellInfo->powerType);
        return SPELL_FAILED_UNKNOWN;
    }

    // Check power amount
    Powers powerType = Powers(m_spellInfo->powerType);
    if (m_caster->GetPower(powerType) < m_powerCost)
        return SPELL_FAILED_NO_POWER;

    return SPELL_CAST_OK;
}

bool Spell::IgnoreItemRequirements() const
{
    if (m_IsTriggeredSpell)
    {
        /// Not own traded item (in trader trade slot) req. reagents including triggered spell case
        if (Item* targetItem = m_targets.getItemTarget())
            if (targetItem->GetOwnerGuid() != m_caster->GetObjectGuid())
                return false;

        /// Some triggered spells have same reagents that have master spell
        /// expected in test: master spell have reagents in first slot then triggered don't must use own
        if (m_triggeredBySpellInfo && !m_triggeredBySpellInfo->Reagent[0])
            return false;

        return true;
    }

    return false;
}

SpellCastResult Spell::CheckItems()
{
    if (m_caster->GetTypeId() != TYPEID_PLAYER)
        return SPELL_CAST_OK;

    Player* p_caster = (Player*)m_caster;

    // cast item checks
    if(m_CastItem)
    {

        //Verify item
        Item *test = p_caster->GetItemByPos( m_CastItem->GetBagSlot(), m_CastItem->GetSlot());
        if(!test || test != m_CastItem || test->GetState() == ITEM_REMOVED)
            return SPELL_FAILED_ITEM_GONE;

        if (m_CastItem->IsInTrade())
            return SPELL_FAILED_ITEM_GONE;

        uint32 itemid = m_CastItem->GetEntry();
        if( !p_caster->HasItemCount(itemid, 1) )
            return SPELL_FAILED_ITEM_NOT_READY;

        ItemPrototype const *proto = m_CastItem->GetProto();
        if(!proto)
            return SPELL_FAILED_ITEM_NOT_READY;

        for (int i = 0; i < 5; ++i)
            if (proto->Spells[i].SpellCharges)
                if(m_CastItem->GetSpellCharges(i) == 0)
                    return SPELL_FAILED_NO_CHARGES_REMAIN;

        // consumable cast item checks
        if (proto->Class == ITEM_CLASS_CONSUMABLE && m_targets.getUnitTarget())
        {
            // such items should only fail if there is no suitable effect at all - see Rejuvenation Potions for example
            SpellCastResult failReason = SPELL_CAST_OK;
            for (int i = 0; i < MAX_EFFECT_INDEX; ++i)
            {
                // skip check, pet not required like checks, and for TARGET_PET m_targets.getUnitTarget() is not the real target but the caster
                if (m_spellInfo->EffectImplicitTargetA[i] == TARGET_PET)
                    continue;

                if (m_spellInfo->Effect[i] == SPELL_EFFECT_HEAL)
                {
                    if (m_targets.getUnitTarget()->GetHealth() == m_targets.getUnitTarget()->GetMaxHealth())
                    {
                        failReason = SPELL_FAILED_ALREADY_AT_FULL_HEALTH;
                        continue;
                    }
                    else
                    {
                        failReason = SPELL_CAST_OK;
                        break;
                    }
                }

                // Mana Potion, Rage Potion, Thistle Tea(Rogue), ...
                if (m_spellInfo->Effect[i] == SPELL_EFFECT_ENERGIZE)
                {
                    if(m_spellInfo->EffectMiscValue[i] < 0 || m_spellInfo->EffectMiscValue[i] >= MAX_POWERS)
                    {
                        failReason = SPELL_FAILED_ALREADY_AT_FULL_MANA;
                        continue;
                    }

                    Powers power = Powers(m_spellInfo->EffectMiscValue[i]);
                    if (m_targets.getUnitTarget()->GetPower(power) == m_targets.getUnitTarget()->GetMaxPower(power))
                    {
                        failReason = SPELL_FAILED_ALREADY_AT_FULL_MANA;
                        continue;
                    }
                    else
                    {
                        failReason = SPELL_CAST_OK;
                        break;
                    }
                }
            }
            if (failReason != SPELL_CAST_OK)
                return failReason;
        }
    }

    // check target item (for triggered case not report error)
    if (m_targets.getItemTargetGuid())
    {
        if (m_caster->GetTypeId() != TYPEID_PLAYER)
            return m_IsTriggeredSpell && !(m_targets.m_targetMask & TARGET_FLAG_TRADE_ITEM)
                   ? SPELL_FAILED_DONT_REPORT : SPELL_FAILED_BAD_TARGETS;

        if (!m_targets.getItemTarget())
            return m_IsTriggeredSpell  && !(m_targets.m_targetMask & TARGET_FLAG_TRADE_ITEM)
                   ? SPELL_FAILED_DONT_REPORT : SPELL_FAILED_ITEM_GONE;

        if (!m_targets.getItemTarget()->IsFitToSpellRequirements(m_spellInfo))
            return m_IsTriggeredSpell  && !(m_targets.m_targetMask & TARGET_FLAG_TRADE_ITEM)
                   ? SPELL_FAILED_DONT_REPORT : SPELL_FAILED_EQUIPPED_ITEM_CLASS;
    }
    // if not item target then required item must be equipped (for triggered case not report error)
    else
    {
        if(m_caster->GetTypeId() == TYPEID_PLAYER && !((Player*)m_caster)->HasItemFitToSpellReqirements(m_spellInfo))
            return m_IsTriggeredSpell ? SPELL_FAILED_DONT_REPORT : SPELL_FAILED_EQUIPPED_ITEM_CLASS;
    }

    // check spell focus object
    if(m_spellInfo->RequiresSpellFocus)
    {
        GameObject* ok = NULL;
        MaNGOS::GameObjectFocusCheck go_check(m_caster,m_spellInfo->RequiresSpellFocus);
        MaNGOS::GameObjectSearcher<MaNGOS::GameObjectFocusCheck> checker(ok, go_check);
        Cell::VisitGridObjects(m_caster, checker, m_caster->GetMap()->GetVisibilityDistance());

        if(!ok)
            return SPELL_FAILED_REQUIRES_SPELL_FOCUS;

        focusObject = ok;                                   // game object found in range
    }

    // check reagents (ignore triggered spells with reagents processed by original spell) and special reagent ignore case.
    if (!IgnoreItemRequirements())
    {
        if (!p_caster->CanNoReagentCast(m_spellInfo))
        {
            for(uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            {
                if(m_spellInfo->Reagent[i] <= 0)
                    continue;

                uint32 itemid    = m_spellInfo->Reagent[i];
                uint32 itemcount = m_spellInfo->ReagentCount[i];

                // if CastItem is also spell reagent
                if (m_CastItem && m_CastItem->GetEntry() == itemid)
                {
                    ItemPrototype const *proto = m_CastItem->GetProto();
                    if (!proto)
                        return SPELL_FAILED_ITEM_NOT_READY;
                    for(int s = 0; s < MAX_ITEM_PROTO_SPELLS; ++s)
                    {
                        // CastItem will be used up and does not count as reagent
                        int32 charges = m_CastItem->GetSpellCharges(s);
                        if (proto->Spells[s].SpellCharges < 0 && !(proto->ExtraFlags & ITEM_EXTRA_NON_CONSUMABLE) && abs(charges) < 2)
                        {
                            ++itemcount;
                            break;
                        }
                    }
                }

                if (!p_caster->HasItemCount(itemid, itemcount))
                    return SPELL_FAILED_ITEM_NOT_READY;
            }
        }

        // check totem-item requirements (items presence in inventory)
        uint32 totems = MAX_SPELL_TOTEMS;
        for(int i = 0; i < MAX_SPELL_TOTEMS ; ++i)
        {
            if (m_spellInfo->Totem[i] != 0)
            {
                if (p_caster->HasItemCount(m_spellInfo->Totem[i], 1))
                {
                    totems -= 1;
                    continue;
                }
            }
            else
                totems -= 1;
        }

        if (totems != 0)
            return SPELL_FAILED_ITEM_GONE;                      //[-ZERO] not sure of it

        /*[-ZERO] to rewrite?
        // Check items for TotemCategory  (items presence in inventory)
        uint32 TotemCategory = MAX_SPELL_TOTEM_CATEGORIES;
        for(int i= 0; i < MAX_SPELL_TOTEM_CATEGORIES; ++i)
        {
            if (m_spellInfo->TotemCategory[i] != 0)
            {
                if (p_caster->HasItemTotemCategory(m_spellInfo->TotemCategory[i]))
                {
                    TotemCategory -= 1;
                    continue;
                }
            }
            else
                TotemCategory -= 1;
        }

        if (TotemCategory != 0)
            return SPELL_FAILED_TOTEM_CATEGORY;                 //0x7B
        */
    }
    // special checks for spell effects
    for(int i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        switch (m_spellInfo->Effect[i])
        {
        case SPELL_EFFECT_CREATE_ITEM:
        {
            if (!m_IsTriggeredSpell && m_spellInfo->EffectItemType[i])
            {
                ItemPosCountVec dest;
                InventoryResult msg = p_caster->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, m_spellInfo->EffectItemType[i], 1 );
                if (msg != EQUIP_ERR_OK )
                {
                    p_caster->SendEquipError( msg, NULL, NULL, m_spellInfo->EffectItemType[i] );
                    return SPELL_FAILED_DONT_REPORT;
                }
            }
            break;
        }
        case SPELL_EFFECT_ENCHANT_ITEM:
        {
            Item* targetItem = m_targets.getItemTarget();
            if(!targetItem)
                return SPELL_FAILED_ITEM_GONE;

            if( targetItem->GetProto()->ItemLevel < m_spellInfo->baseLevel )
                return SPELL_FAILED_LOWLEVEL;
            // Not allow enchant in trade slot for some enchant type
            if( targetItem->GetOwner() != m_caster )
            {
                uint32 enchant_id = m_spellInfo->EffectMiscValue[i];
                SpellItemEnchantmentEntry const *pEnchant = sSpellItemEnchantmentStore.LookupEntry(enchant_id);
                if(!pEnchant)
                    return SPELL_FAILED_ERROR;
                if (pEnchant->slot & ENCHANTMENT_CAN_SOULBOUND)
                    return SPELL_FAILED_NOT_TRADEABLE;
            }
            break;
        }
        case SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY:
        {
            Item *item = m_targets.getItemTarget();
            if(!item)
                return SPELL_FAILED_ITEM_GONE;
            // Not allow enchant in trade slot for some enchant type
            if( item->GetOwner() != m_caster )
            {
                uint32 enchant_id = m_spellInfo->EffectMiscValue[i];
                SpellItemEnchantmentEntry const *pEnchant = sSpellItemEnchantmentStore.LookupEntry(enchant_id);
                if(!pEnchant)
                    return SPELL_FAILED_ERROR;
                if (pEnchant->slot & ENCHANTMENT_CAN_SOULBOUND)
                    return SPELL_FAILED_NOT_TRADEABLE;
            }
            break;
        }
        case SPELL_EFFECT_ENCHANT_HELD_ITEM:
            // check item existence in effect code (not output errors at offhand hold item effect to main hand for example
            break;
        case SPELL_EFFECT_DISENCHANT:
        {
            if(!m_targets.getItemTarget())
                return SPELL_FAILED_CANT_BE_DISENCHANTED;

            // prevent disenchanting in trade slot
            if( m_targets.getItemTarget()->GetOwnerGuid() != m_caster->GetObjectGuid() )
                return SPELL_FAILED_CANT_BE_DISENCHANTED;

            ItemPrototype const* itemProto = m_targets.getItemTarget()->GetProto();
            if(!itemProto)
                return SPELL_FAILED_CANT_BE_DISENCHANTED;

            // must have disenchant loot (other static req. checked at item prototype loading)
            if (!itemProto->DisenchantID)
                return SPELL_FAILED_CANT_BE_DISENCHANTED;
            break;
        }
        case SPELL_EFFECT_WEAPON_DAMAGE:
        case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
        {
            if(m_caster->GetTypeId() != TYPEID_PLAYER) return SPELL_FAILED_TARGET_NOT_PLAYER;
            if( m_attackType != RANGED_ATTACK )
                break;
            Item *pItem = ((Player*)m_caster)->GetWeaponForAttack(m_attackType,true,false);
            if (!pItem)
                return SPELL_FAILED_EQUIPPED_ITEM;

            switch(pItem->GetProto()->SubClass)
            {
            case ITEM_SUBCLASS_WEAPON_THROWN:
            {
                uint32 ammo = pItem->GetEntry();
                if( !((Player*)m_caster)->HasItemCount( ammo, 1 ) )
                    return SPELL_FAILED_NO_AMMO;
            };
            break;
            case ITEM_SUBCLASS_WEAPON_GUN:
            case ITEM_SUBCLASS_WEAPON_BOW:
            case ITEM_SUBCLASS_WEAPON_CROSSBOW:
            {
                uint32 ammo = ((Player*)m_caster)->GetUInt32Value(PLAYER_AMMO_ID);
                if(!ammo)
                {
                    // Requires No Ammo
                    if(m_caster->GetDummyAura(46699))
                        break;                      // skip other checks

                    return SPELL_FAILED_NO_AMMO;
                }

                ItemPrototype const *ammoProto = ObjectMgr::GetItemPrototype( ammo );
                if(!ammoProto)
                    return SPELL_FAILED_NO_AMMO;

                if(ammoProto->Class != ITEM_CLASS_PROJECTILE)
                    return SPELL_FAILED_NO_AMMO;

                // check ammo ws. weapon compatibility
                switch(pItem->GetProto()->SubClass)
                {
                case ITEM_SUBCLASS_WEAPON_BOW:
                case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                    if(ammoProto->SubClass != ITEM_SUBCLASS_ARROW)
                        return SPELL_FAILED_NO_AMMO;
                    break;
                case ITEM_SUBCLASS_WEAPON_GUN:
                    if(ammoProto->SubClass != ITEM_SUBCLASS_BULLET)
                        return SPELL_FAILED_NO_AMMO;
                    break;
                default:
                    return SPELL_FAILED_NO_AMMO;
                }

                if( !((Player*)m_caster)->HasItemCount( ammo, 1 ) )
                    return SPELL_FAILED_NO_AMMO;
            };
            break;
            case ITEM_SUBCLASS_WEAPON_WAND:
                break;
            default:
                break;
            }
            break;
        }
        default:
            break;
        }
    }

    return SPELL_CAST_OK;
}

void Spell::Delayed()
{
    if(!m_caster || m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    if (m_spellState == SPELL_STATE_DELAYED)
        return;                                             // spell is active and can't be time-backed

    // spells not loosing casting time ( slam, dynamites, bombs.. )
    if(!(m_spellInfo->InterruptFlags & SPELL_INTERRUPT_FLAG_DAMAGE))
        return;

    //check resist chance
    int32 resistChance = 100;                               //must be initialized to 100 for percent modifiers
    ((Player*)m_caster)->ApplySpellMod(m_spellInfo->Id, SPELLMOD_NOT_LOSE_CASTING_TIME, resistChance, this);
    resistChance += m_caster->GetTotalAuraModifier(SPELL_AURA_RESIST_PUSHBACK) - 100;
    if (roll_chance_i(resistChance))
        return;

    int32 delaytime = GetNextDelayAtDamageMsTime();

    if(int32(m_timer) + delaytime > m_casttime)
    {
        delaytime = m_casttime - m_timer;
        m_timer = m_casttime;
    }
    else
        m_timer += delaytime;

    DETAIL_FILTER_LOG(LOG_FILTER_SPELL_CAST, "Spell %u partially interrupted for (%d) ms at damage", m_spellInfo->Id, delaytime);

    WorldPacket data(SMSG_SPELL_DELAYED, 8+4);
    data << ObjectGuid(m_caster->GetObjectGuid());
    data << uint32(delaytime);

    if (m_caster->GetTypeId() == TYPEID_PLAYER)
        ((Player*)m_caster)->SendDirectMessage(&data);
}

void Spell::DelayedChannel()
{
    if(!m_caster || m_caster->GetTypeId() != TYPEID_PLAYER || getState() != SPELL_STATE_CASTING)
        return;

    //check resist chance
    int32 resistChance = 100;                               //must be initialized to 100 for percent modifiers
    ((Player*)m_caster)->ApplySpellMod(m_spellInfo->Id, SPELLMOD_NOT_LOSE_CASTING_TIME, resistChance, this);
    resistChance += m_caster->GetTotalAuraModifier(SPELL_AURA_RESIST_PUSHBACK) - 100;
    if (roll_chance_i(resistChance))
        return;

    int32 delaytime = GetNextDelayAtDamageMsTime();

    if(int32(m_timer) < delaytime)
    {
        delaytime = m_timer;
        m_timer = 0;
    }
    else
        m_timer -= delaytime;

    DEBUG_FILTER_LOG(LOG_FILTER_SPELL_CAST, "Spell %u partially interrupted for %i ms, new duration: %u ms", m_spellInfo->Id, delaytime, m_timer);

    for(TargetList::const_iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        if ((*ihit).missCondition == SPELL_MISS_NONE)
        {
            if (Unit* unit = m_caster->GetObjectGuid() == ihit->targetGUID ? m_caster : ObjectAccessor::GetUnit(*m_caster, ihit->targetGUID))
                unit->DelaySpellAuraHolder(m_spellInfo->Id, delaytime, unit->GetObjectGuid());
        }
    }

    for(int j = 0; j < MAX_EFFECT_INDEX; ++j)
    {
        // partially interrupt persistent area auras
        if (DynamicObject* dynObj = m_caster->GetDynObject(m_spellInfo->Id, SpellEffectIndex(j)))
            dynObj->Delay(delaytime);
    }

    SendChannelUpdate(m_timer);
}

void Spell::UpdateOriginalCasterPointer()
{
    if(m_originalCasterGUID == m_caster->GetObjectGuid())
        m_originalCaster = m_caster;
    else if (m_originalCasterGUID.IsGameObject())
    {
        GameObject* go = m_caster->IsInWorld() ? m_caster->GetMap()->GetGameObject(m_originalCasterGUID) : NULL;
        m_originalCaster = go ? go->GetOwner() : NULL;
    }
    else
    {
        Unit* unit = ObjectAccessor::GetUnit(*m_caster, m_originalCasterGUID);
        m_originalCaster = unit && unit->IsInWorld() ? unit : NULL;
    }
}

void Spell::UpdatePointers()
{
    UpdateOriginalCasterPointer();

    m_targets.Update(m_caster);
}

bool Spell::CheckTargetCreatureType(Unit* target) const
{
    uint32 spellCreatureTargetMask = m_spellInfo->TargetCreatureType;

    // Curse of Doom : not find another way to fix spell target check :/
    if (m_spellInfo->Id == 603)                             // in 1.12 "Curse of doom" have only 1 rank.
    {
        // not allow cast at player
        if(target->GetTypeId() == TYPEID_PLAYER)
            return false;

        spellCreatureTargetMask = 0x7FF;
    }

    // Dismiss Pet and Taming Lesson skipped
    if(m_spellInfo->Id == 2641 || m_spellInfo->Id == 23356)
        spellCreatureTargetMask =  0;

    if (spellCreatureTargetMask)
    {
        uint32 TargetCreatureType = target->GetCreatureTypeMask();

        return !TargetCreatureType || (spellCreatureTargetMask & TargetCreatureType);
    }
    return true;
}

CurrentSpellTypes Spell::GetCurrentContainer()
{
    if (IsNextMeleeSwingSpell())
        return(CURRENT_MELEE_SPELL);
    else if (IsAutoRepeat())
        return(CURRENT_AUTOREPEAT_SPELL);
    else if (IsChanneledSpell(m_spellInfo))
        return(CURRENT_CHANNELED_SPELL);
    else
        return(CURRENT_GENERIC_SPELL);
}

bool Spell::CheckTarget( Unit* target, SpellEffectIndex eff )
{
    // Check targets for creature type mask and remove not appropriate (skip explicit self target case, maybe need other explicit targets)
    if(m_spellInfo->EffectImplicitTargetA[eff] != TARGET_SELF )
    {
        if (!CheckTargetCreatureType(target))
            return false;
    }

    // Check targets for not_selectable unit flag and remove
    // A player can cast spells on his pet (or other controlled unit) though in any state
    if (target != m_caster && target->GetCharmerOrOwnerGuid() != m_caster->GetObjectGuid())
    {
        // any unattackable target skipped
        if (target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE) &&
            (target->GetTypeId() != TYPEID_UNIT || !dynamic_cast<Creature*>(target)->GetIgnoreNonCombatFlags()))
            return false;

        // unselectable targets skipped in all cases except TARGET_SCRIPT targeting
        // in case TARGET_SCRIPT target selected by server always and can't be cheated
        if ((!m_IsTriggeredSpell || target != m_targets.getUnitTarget()) &&
                target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE) &&
                m_spellInfo->EffectImplicitTargetA[eff] != TARGET_SCRIPT &&
                m_spellInfo->EffectImplicitTargetB[eff] != TARGET_SCRIPT &&
                m_spellInfo->EffectImplicitTargetA[eff] != TARGET_AREAEFFECT_INSTANT &&
                m_spellInfo->EffectImplicitTargetB[eff] != TARGET_AREAEFFECT_INSTANT &&
                m_spellInfo->EffectImplicitTargetA[eff] != TARGET_AREAEFFECT_CUSTOM &&
                m_spellInfo->EffectImplicitTargetB[eff] != TARGET_AREAEFFECT_CUSTOM )
            return false;
    }

    // Check player targets and remove if in GM mode or GM invisibility (for not self casting case)
    if( target != m_caster && target->GetTypeId() == TYPEID_PLAYER)
    {
        if(((Player*)target)->GetVisibility() == VISIBILITY_OFF)
            return false;

        if(((Player*)target)->isGameMaster() && !IsPositiveSpell(m_spellInfo->Id))
            return false;
    }

    // Check targets for LOS visibility (except spells without range limitations )
    switch(m_spellInfo->Effect[eff])
    {
    case SPELL_EFFECT_SUMMON_PLAYER:                    // from anywhere
        break;
    case SPELL_EFFECT_SKIN_PLAYER_CORPSE:
        if (Corpse *corpse = m_caster->GetMap()->GetCorpse(m_targets.getCorpseTargetGuid()))
        {
            if (corpse->GetOwnerGuid() != target->GetObjectGuid())
                return false;
        } else
        {
            return false;
        }
        break;
    case SPELL_EFFECT_DUMMY:
        if(m_spellInfo->Id != 20577)                    // Cannibalize
            break;
        // fall through
    case SPELL_EFFECT_RESURRECT_NEW:
        // player far away, maybe his corpse near?
        if(target != m_caster && !target->IsWithinLOSInMap(m_caster))
        {
            if (!m_targets.getCorpseTargetGuid())
                return false;

            Corpse *corpse = m_caster->GetMap()->GetCorpse(m_targets.getCorpseTargetGuid());
            if(!corpse)
                return false;

            if(target->GetObjectGuid() != corpse->GetOwnerGuid())
                return false;

            if(!corpse->IsWithinLOSInMap(m_caster))
                return false;
        }

        // all ok by some way or another, skip normal check
        break;
    default:                                            // normal case
        // Get GO cast coordinates if original caster -> GO
        //RangeIndex 13 is "everywhere" and shouldn't check LOS
        if (target != m_caster && m_spellInfo->rangeIndex != 13)
            if (WorldObject *caster = GetCastingObject())
            {
                switch (m_spellInfo->Id)
                {
                case 19703:			// Lucifron's Curse
                case 19702:			// Impending Doom
                case 28599:			// Shadowbolt Volley
                case 22682:			// Shadow Flame debuff
                case 23410:			// Nefarian's class calls.
                case 23397:
                case 23398:
                case 23401:
                case 23418:
                case 23425:
                case 23427:
                case 23436:
                case 23414:
                    break;
                default:
                    if (!target->IsWithinLOSInMap(caster))
                        return false;
                    break;
                }
            }
        break;
    }

    return true;
}

bool Spell::IsNeedSendToClient() const
{
    return m_spellInfo->SpellVisual!=0 || IsChanneledSpell(m_spellInfo) ||
           m_spellInfo->speed > 0.0f || (!m_triggeredByAuraSpell && !m_IsTriggeredSpell);
}

bool Spell::IsExceptedFromClientUpdate() const
{
    switch (m_spellInfo->Id)
    {
    case 22247: // Suppression Aura
        return true;
    default:
        return false;
    }
}


bool Spell::IsTriggeredSpellWithRedundentData() const
{
    return m_triggeredByAuraSpell || m_triggeredBySpellInfo ||
           // possible not need after above check?
           (m_IsTriggeredSpell && (m_spellInfo->manaCost || m_spellInfo->ManaCostPercentage));
}

bool Spell::HaveTargetsForEffect(SpellEffectIndex effect) const
{
    for(TargetList::const_iterator itr = m_UniqueTargetInfo.begin(); itr != m_UniqueTargetInfo.end(); ++itr)
        if(itr->effectMask & (1 << effect))
            return true;

    for(GOTargetList::const_iterator itr = m_UniqueGOTargetInfo.begin(); itr != m_UniqueGOTargetInfo.end(); ++itr)
        if(itr->effectMask & (1 << effect))
            return true;

    for(ItemTargetList::const_iterator itr = m_UniqueItemInfo.begin(); itr != m_UniqueItemInfo.end(); ++itr)
        if(itr->effectMask & (1 << effect))
            return true;

    return false;
}

SpellEvent::SpellEvent(Spell* spell) : BasicEvent()
{
    m_Spell = spell;
}

SpellEvent::~SpellEvent()
{
    if (m_Spell->getState() != SPELL_STATE_FINISHED)
        m_Spell->cancel();

    if (m_Spell->IsDeletable())
    {
        delete m_Spell;
    }
    else
    {
        sLog.outError("~SpellEvent: %s %u tried to delete non-deletable spell %u. Was not deleted, causes memory leak.",
                      (m_Spell->GetCaster()->GetTypeId() == TYPEID_PLAYER ? "Player" : "Creature"), m_Spell->GetCaster()->GetGUIDLow(), m_Spell->m_spellInfo->Id);
    }
}

bool SpellEvent::Execute(uint64 e_time, uint32 p_time)
{
    // update spell if it is not finished
    if (m_Spell->getState() != SPELL_STATE_FINISHED)
        m_Spell->update(p_time);

    // check spell state to process
    switch (m_Spell->getState())
    {
    case SPELL_STATE_FINISHED:
    {
        // spell was finished, check deletable state
        if (m_Spell->IsDeletable())
        {
            // check, if we do have unfinished triggered spells
            return true;                                // spell is deletable, finish event
        }
        // event will be re-added automatically at the end of routine)
    }
    break;

    case SPELL_STATE_CASTING:
    {
        // this spell is in channeled state, process it on the next update
        // event will be re-added automatically at the end of routine)
    } break;

    case SPELL_STATE_DELAYED:
    {
        // first, check, if we have just started
        if (m_Spell->GetDelayStart() != 0)
        {
            // no, we aren't, do the typical update
            // check, if we have channeled spell on our hands
            if (IsChanneledSpell(m_Spell->m_spellInfo))
            {
                // evented channeled spell is processed separately, casted once after delay, and not destroyed till finish
                // check, if we have casting anything else except this channeled spell and autorepeat
                if (m_Spell->GetCaster()->IsNonMeleeSpellCasted(false, true, true))
                {
                    // another non-melee non-delayed spell is casted now, abort
                    m_Spell->cancel();
                }
                else
                {
                    // do the action (pass spell to channeling state)
                    m_Spell->handle_immediate();
                }
                // event will be re-added automatically at the end of routine)
            }
            else
            {
                // run the spell handler and think about what we can do next
                uint64 t_offset = e_time - m_Spell->GetDelayStart();
                uint64 n_offset = m_Spell->handle_delayed(t_offset);
                if (n_offset)
                {
                    // re-add us to the queue
                    m_Spell->GetCaster()->m_Events.AddEvent(this, m_Spell->GetDelayStart() + n_offset, false);
                    return false;                       // event not complete
                }
                // event complete
                // finish update event will be re-added automatically at the end of routine)
            }
        }
        else
        {
            // delaying had just started, record the moment
            m_Spell->SetDelayStart(e_time);
            // re-plan the event for the delay moment
            m_Spell->GetCaster()->m_Events.AddEvent(this, e_time + m_Spell->GetDelayMoment(), false);
            return false;                               // event not complete
        }
    }
    break;

    default:
    {
        // all other states
        // event will be re-added automatically at the end of routine)
    } break;
    }

    // spell processing not complete, plan event on the next update interval
    m_Spell->GetCaster()->m_Events.AddEvent(this, e_time + 1, false);
    return false;                                           // event not complete
}

void SpellEvent::Abort(uint64 /*e_time*/)
{
    // oops, the spell we try to do is aborted
    if (m_Spell->getState() != SPELL_STATE_FINISHED)
        m_Spell->cancel();
}

bool SpellEvent::IsDeletable() const
{
    return m_Spell->IsDeletable();
}

SpellCastResult Spell::CanOpenLock(SpellEffectIndex effIndex, uint32 lockId, SkillType& skillId, int32& reqSkillValue, int32& skillValue)
{
    if(!lockId)                                             // possible case for GO and maybe for items.
        return SPELL_CAST_OK;

    // Get LockInfo
    LockEntry const *lockInfo = sLockStore.LookupEntry(lockId);

    if (!lockInfo)
        return SPELL_FAILED_BAD_TARGETS;

    bool reqKey = false;                                    // some locks not have reqs

    for(int j = 0; j < 8; ++j)
    {
        switch(lockInfo->Type[j])
        {
            // check key item (many fit cases can be)
        case LOCK_KEY_ITEM:
            if(lockInfo->Index[j] && m_CastItem && m_CastItem->GetEntry()==lockInfo->Index[j])
                return SPELL_CAST_OK;
            reqKey = true;
            break;
            // check key skill (only single first fit case can be)
        case LOCK_KEY_SKILL:
        {
            reqKey = true;

            // wrong locktype, skip
            if(uint32(m_spellInfo->EffectMiscValue[effIndex]) != lockInfo->Index[j])
                continue;

            skillId = SkillByLockType(LockType(lockInfo->Index[j]));

            if ( skillId != SKILL_NONE )
            {
                // skill bonus provided by casting spell (mostly item spells)
                // add the damage modifier from the spell casted (cheat lock / skeleton key etc.) (use m_currentBasePoints, CalculateDamage returns wrong value)
                uint32 spellSkillBonus = uint32(m_currentBasePoints[effIndex]);
                reqSkillValue = lockInfo->Skill[j];

                // castitem check: rogue using skeleton keys. the skill values should not be added in this case.
                skillValue = m_CastItem || m_caster->GetTypeId()!= TYPEID_PLAYER ?
                             0 : ((Player*)m_caster)->GetSkillValue(skillId);

                skillValue += spellSkillBonus;

                if (skillValue < reqSkillValue)
                    return SPELL_FAILED_SKILL_NOT_HIGH_ENOUGH;
            }

            return SPELL_CAST_OK;
        }
        }
    }

    if(reqKey)
        return SPELL_FAILED_BAD_TARGETS;

    return SPELL_CAST_OK;
}

/**
 * Fill target list by units around (x,y) points at radius distance

 * @param targetUnitMap        Reference to target list that filled by function
 * @param x                    X coordinates of center point for target search
 * @param y                    Y coordinates of center point for target search
 * @param radius               Radius around (x,y) for target search
 * @param pushType             Additional rules for target area selection (in front, angle, etc)
 * @param spellTargets         Additional rules for target selection base at hostile/friendly state to original spell caster
 * @param originalCaster       If provided set alternative original caster, if =NULL then used Spell::GetAffectiveObject() return
 */
void Spell::FillAreaTargets(UnitList &targetUnitMap, float radius, SpellNotifyPushType pushType, SpellTargets spellTargets, WorldObject* originalCaster /*=NULL*/)
{
    MaNGOS::SpellNotifierCreatureAndPlayer notifier(*this, targetUnitMap, radius, pushType, spellTargets, originalCaster);
    Cell::VisitAllObjects(notifier.GetCenterX(), notifier.GetCenterY(), m_caster->GetMap(), notifier, radius);
}

void Spell::FillRaidOrPartyTargets( UnitList &TagUnitMap, Unit* target, float radius, bool raid, bool withPets, bool withcaster )
{
    Player *pTarget = target->GetCharmerOrOwnerPlayerOrPlayerItself();
    Group *pGroup = pTarget ? pTarget->GetGroup() : NULL;

    if (pGroup)
    {
        uint8 subgroup = pTarget->GetSubGroup();

        for(GroupReference *itr = pGroup->GetFirstMember(); itr != NULL; itr = itr->next())
        {
            Player* Target = itr->getSource();

            // IsHostileTo check duel and controlled by enemy
            if (Target && (raid || subgroup==Target->GetSubGroup())
                    && !m_caster->IsHostileTo(Target))
            {
                if ((Target==m_caster && withcaster) ||
                        (Target!=m_caster && m_caster->IsWithinDistInMap(Target, radius)))
                    TagUnitMap.push_back(Target);

                if (withPets)
                    if (Pet* pet = Target->GetPet())
                        if ((pet==m_caster && withcaster) ||
                                (pet!=m_caster && m_caster->IsWithinDistInMap(pet, radius)))
                            TagUnitMap.push_back(pet);
            }
        }
    }
    else
    {
        Unit* ownerOrSelf = pTarget ? pTarget : target->GetCharmerOrOwnerOrSelf();
        if ((ownerOrSelf==m_caster && withcaster) ||
                (ownerOrSelf!=m_caster && m_caster->IsWithinDistInMap(ownerOrSelf, radius)))
            TagUnitMap.push_back(ownerOrSelf);

        if (withPets)
            if (Pet* pet = ownerOrSelf->GetPet())
                if ((pet==m_caster && withcaster) ||
                        (pet!=m_caster && m_caster->IsWithinDistInMap(pet, radius)))
                    TagUnitMap.push_back(pet);
    }
}

WorldObject* Spell::GetAffectiveCasterObject() const
{
    if (!m_originalCasterGUID)
        return m_caster;

    if (m_originalCasterGUID.IsGameObject() && m_caster->IsInWorld())
        return m_caster->GetMap()->GetGameObject(m_originalCasterGUID);
    return m_originalCaster;
}

WorldObject* Spell::GetCastingObject() const
{
    if (m_originalCasterGUID.IsGameObject())
        return m_caster->IsInWorld() ? m_caster->GetMap()->GetGameObject(m_originalCasterGUID) : NULL;
    else
        return m_caster;
}

void Spell::ClearCastItem()
{
    if (m_CastItem==m_targets.getItemTarget())
        m_targets.setItemTarget(NULL);

    m_CastItem = NULL;
}

bool Spell::HasGlobalCooldown()
{
    // global cooldown have only player or controlled units
    if (m_caster->GetCharmInfo())
        return m_caster->GetCharmInfo()->GetGlobalCooldownMgr().HasGlobalCooldown(m_spellInfo);
    else if (m_caster->GetTypeId() == TYPEID_PLAYER)
        return ((Player*)m_caster)->GetGlobalCooldownMgr().HasGlobalCooldown(m_spellInfo);
    else
        return false;
}

void Spell::TriggerGlobalCooldown()
{
    int32 gcd = m_spellInfo->StartRecoveryTime;
    if (!gcd)
        return;

    // global cooldown can't leave range 1..1.5 secs (if it it)
    // exist some spells (mostly not player directly casted) that have < 1 sec and > 1.5 sec global cooldowns
    // but its as test show not affected any spell mods.
    if (gcd >= 1000 && gcd <= 1500)
    {
        // apply haste rating
        gcd = int32(float(gcd) * m_caster->GetFloatValue(UNIT_MOD_CAST_SPEED));

        if (gcd < 1000)
            gcd = 1000;
        else if (gcd > 1500)
            gcd = 1500;
    }

    // global cooldown have only player or controlled units
    if (m_caster->GetCharmInfo())
        m_caster->GetCharmInfo()->GetGlobalCooldownMgr().AddGlobalCooldown(m_spellInfo, gcd);
    else if (m_caster->GetTypeId() == TYPEID_PLAYER)
        ((Player*)m_caster)->GetGlobalCooldownMgr().AddGlobalCooldown(m_spellInfo, gcd);
}

void Spell::CancelGlobalCooldown()
{
    if (!m_spellInfo->StartRecoveryTime)
        return;

    // cancel global cooldown when interrupting current cast
    if (m_caster->GetCurrentSpell(CURRENT_GENERIC_SPELL) != this)
        return;

    // global cooldown have only player or controlled units
    if (m_caster->GetCharmInfo())
        m_caster->GetCharmInfo()->GetGlobalCooldownMgr().CancelGlobalCooldown(m_spellInfo);
    else if (m_caster->GetTypeId() == TYPEID_PLAYER)
        ((Player*)m_caster)->GetGlobalCooldownMgr().CancelGlobalCooldown(m_spellInfo);
}

void Spell::ResetEffectDamageAndHeal()
{
    m_damage = 0;
    m_healing = 0;
}
