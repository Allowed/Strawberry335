/*
* Copyright (C) 2008-2014 TrinityCore <http://www.trinitycore.org/>
* Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
*
* This program is free software; you can redistribute it and/or modify it
* under the terms of the GNU General Public License as published by the
* Free Software Foundation; either version 2 of the License, or (at your
* option) any later version.
*
* This program is distributed in the hope that it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
* more details.
*
* You should have received a copy of the GNU General Public License along
* with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "Common.h"
#include "Transport.h"
#include "MapManager.h"
#include "ObjectMgr.h"
#include "Path.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "DBCStores.h"
#include "World.h"
#include "GameObjectAI.h"
#include "Vehicle.h"
#include "MapReference.h"
#include "Player.h"
#include "Cell.h"
#include "CellImpl.h"
#include "Totem.h"

Transport::Transport() : GameObject(),
_transportInfo(NULL), _isMoving(true), _pendingStop(false),
_passengerTeleportItr(_passengers.begin()), _delayedAddModel(false)
{
    m_updateFlag = UPDATEFLAG_TRANSPORT | UPDATEFLAG_LOWGUID | UPDATEFLAG_STATIONARY_POSITION | UPDATEFLAG_ROTATION;
}

Transport::~Transport()
{
    ASSERT(_passengers.empty());
    UnloadStaticPassengers();
}

bool Transport::Create(uint32 guidlow, uint32 entry, uint32 mapid, float x, float y, float z, float ang, uint32 animprogress)
{
    Relocate(x, y, z, ang);

    if (!IsPositionValid())
    {
        sLog->outError("Transport (GUID: %u) not created. Suggested coordinates isn't valid (X: %f Y: %f)",
                     guidlow, x, y);
        return false;
    }

    Object::_Create(guidlow, 0, HIGHGUID_MO_TRANSPORT);

    GameObjectTemplate const* goinfo = sObjectMgr->GetGameObjectTemplate(entry);

    if (!goinfo)
    {
        sLog->outError("Transport not created: entry in `gameobject_template` not found, guidlow: %u map: %u  (X: %f Y: %f Z: %f) ang: %f", guidlow, mapid, x, y, z, ang);
        return false;
    }

    m_goInfo = goinfo;

    TransportTemplate const* tInfo = sTransportMgr->GetTransportTemplate(entry);
    if (!tInfo)
    {
        sLog->outError("Transport %u (name: %s) will not be created, missing `transport_template` entry.", entry, goinfo->name.c_str());
        return false;
    }

    _transportInfo = tInfo;

    // initialize waypoints
    _nextFrame = tInfo->keyFrames.begin();
    _currentFrame = _nextFrame++;

    m_goValue->Transport.PathProgress = 0;
    SetObjectScale(goinfo->size);
    SetFaction(goinfo->faction);
    SetUInt32Value(GAMEOBJECT_FLAGS, goinfo->flags);
    SetPeriod(tInfo->pathTime);
    SetEntry(goinfo->entry);
    SetDisplayId(goinfo->displayId);
    SetGoState(!goinfo->moTransport.canBeStopped ? GO_STATE_READY : GO_STATE_ACTIVE);
    SetGoType(GAMEOBJECT_TYPE_MO_TRANSPORT);
    SetGoAnimProgress(animprogress);
    SetName(goinfo->name);
    UpdateRotationFields(0.0f, 1.0f);

    m_model = GameObjectModel::Create(*this);
    return true;
}

void Transport::CleanupsBeforeDelete(bool finalCleanup /*= true*/)
{
    UnloadStaticPassengers();
    while (!_passengers.empty())
    {
        WorldObject* obj = *_passengers.begin();
        RemovePassenger(obj);
    }

    GameObject::CleanupsBeforeDelete(finalCleanup);
}

void Transport::Update(uint32 diff)
{
    if (AI())
        AI()->UpdateAI(diff);
    else if (!AIM_Initialize())
        sLog->outError("Could not initialize GameObjectAI for Transport");

    if (GetKeyFrames().size() <= 1)
        return;

    if (IsMoving() || !_pendingStop)
        m_goValue->Transport.PathProgress += diff;

    uint32 timer = m_goValue->Transport.PathProgress % GetPeriod();
    bool stopFrame = false; // Reached stop frame => No Movement
    bool nextWaypoint = true; // Waypoint reached => Get next

    //sLog->outError("Timer %u", timer);
    //sLog->outError("[%u] ArriveTime %u, NextArriveTime %u DepartureTime %u", _currentFrame->Index, _currentFrame->ArriveTime, _currentFrame->NextArriveTime, _currentFrame->DepartureTime);

    // Arrival
    if (timer >= _currentFrame->ArriveTime && timer < _currentFrame->DepartureTime)
    {
        if (IsMoving())
        {
            // Event
            DoEventIfAny(*_currentFrame, false);

            // Disable Movement
            SetMoving(false);

            if (_pendingStop && GetGoState() != GO_STATE_READY)
            {
                SetGoState(GO_STATE_READY);
                m_goValue->Transport.PathProgress = (m_goValue->Transport.PathProgress / GetPeriod());
                m_goValue->Transport.PathProgress *= GetPeriod();
                m_goValue->Transport.PathProgress += _currentFrame->ArriveTime;
            }
        }

        stopFrame = true;
        nextWaypoint = false;
    }

    // Departure / During Movement
    if (!stopFrame)
    {
        if (!IsMoving())
        {
            // Event
            DoEventIfAny(*_currentFrame, true);

            // Enable movement
            SetMoving(true);

            if (GetGOInfo()->moTransport.canBeStopped)
                SetGoState(GO_STATE_ACTIVE);
        }

        if (timer >= _currentFrame->DepartureTime && timer < _currentFrame->NextArriveTime)
            nextWaypoint = false;
    }

    // Reached Waypoint => Move to next
    if (nextWaypoint)
    {
        MoveToNextWaypoint();

        sScriptMgr->OnRelocate(this, _currentFrame->Node->index, _currentFrame->Node->mapid, _currentFrame->Node->x, _currentFrame->Node->y, _currentFrame->Node->z);

        sLog->outDebug(LOG_FILTER_TRANSPORTS, "Transport %u (%s) moved to node %u %u %f %f %f", GetEntry(), GetName(), _currentFrame->Node->index, _currentFrame->Node->mapid, _currentFrame->Node->x, _currentFrame->Node->y, _currentFrame->Node->z);

        if (_currentFrame->IsTeleportFrame()) // Teleport frame reached
            TeleportTransport(_nextFrame->Node->mapid, _nextFrame->Node->x, _nextFrame->Node->y, _nextFrame->Node->z, _nextFrame->InitialOrientation);
        return;
    }

    if (_delayedAddModel)
    {
        _delayedAddModel = false;
        if (m_model)
            GetMap()->Insert(*m_model);
    }

    // Set position
    _positionChangeTimer.Update(diff);
    if (_positionChangeTimer.Passed())
    {
        _positionChangeTimer.Reset(positionUpdateDelay);

        if (IsMoving())
        {
            float t = CalculateSegmentPos(float(timer) * 0.001f);
            G3D::Vector3 pos, dir;
            _currentFrame->Spline->evaluate_percent(_currentFrame->Index, t, pos);
            _currentFrame->Spline->evaluate_derivative(_currentFrame->Index, t, dir);
            UpdatePosition(pos.x, pos.y, pos.z, std::atan2(dir.y, dir.x) + float(M_PI));
        }
        else
        {
            bool gridActive = GetMap()->IsGridLoaded(GetPositionX(), GetPositionY());

            if (_staticPassengers.empty() && gridActive)
                LoadStaticPassengers();
            else if (!_staticPassengers.empty() && !gridActive)
                UnloadStaticPassengers();
        }
    }

    sScriptMgr->OnTransportUpdate(this, diff);
}

void Transport::AddPassenger(WorldObject* passenger)
{
    if (!IsInWorld())
        return;

    if (_passengers.insert(passenger).second)
    {
        passenger->SetTransport(this);
        passenger->m_movementInfo.AddMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
        passenger->m_movementInfo.transport.guid = GetGUID();
        sLog->outDebug(LOG_FILTER_TRANSPORTS, "Object %s boarded transport %s.", passenger->GetName(), GetName());

        if (Player* plr = passenger->ToPlayer())
            sScriptMgr->OnAddPassenger(this, plr);
    }
}

void Transport::RemovePassenger(WorldObject* passenger)
{
    bool erased = false;
    if (_passengerTeleportItr != _passengers.end())
    {
        PassengerSet::iterator itr = _passengers.find(passenger);
        if (itr != _passengers.end())
        {
            if (itr == _passengerTeleportItr)
                ++_passengerTeleportItr;

            _passengers.erase(itr);
            erased = true;
        }
    }
    else
        erased = _passengers.erase(passenger) > 0;

    if (erased || _staticPassengers.erase(passenger)) // static passenger can remove itself in case of grid unload
    {
        passenger->SetTransport(NULL);
        passenger->m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
        passenger->m_movementInfo.transport.Reset();
        sLog->outDebug(LOG_FILTER_TRANSPORTS, "Object %s removed from transport %s.", passenger->GetName(), GetName());

        if (Player* plr = passenger->ToPlayer())
            sScriptMgr->OnRemovePassenger(this, plr);
    }
}

Creature* Transport::CreateNPCPassenger(uint32 guid, CreatureData const* data)
{
    sLog->outDebug(LOG_FILTER_TRANSPORTS, "Transport::CreateNPCPassenger: Create Creature lowguid %u", guid);

    Map* map = GetMap();
    Creature* creature = new Creature();

    if (!creature->LoadCreatureFromDB(guid, map, false))
    {
        sLog->outError("Transport::CreateNPCPassenger: Creature lowguid %u not loaded. Something went wrong in LoadCreatureFromDB)", guid);
        delete creature;
        return NULL;
    }

    float x = data->posX;
    float y = data->posY;
    float z = data->posZ;
    float o = data->orientation;

    creature->SetTransport(this);
    creature->AddUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
    creature->m_movementInfo.transport.guid = GetGUID();
    creature->m_movementInfo.transport.pos.Relocate(x, y, z, o);
    CalculatePassengerPosition(x, y, z, o);
    creature->Relocate(x, y, z, o);
    creature->SetHomePosition(creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ(), creature->GetOrientation());
    creature->SetTransportHomePosition(creature->m_movementInfo.transport.pos);

    /// @HACK - transport models are not added to map's dynamic LoS calculations
    ///         because the current GameObjectModel cannot be moved without recreating - no mmaps
    /// creature->AddUnitState(UNIT_STATE_IGNORE_PATHFINDING);

    if (!creature->IsPositionValid())
    {
        sLog->outError("Transport::CreateNPCPassenger: Creature (guidlow %d, entry %d) not created. Suggested coordinates aren't valid (X: %f Y: %f)",
                       creature->GetGUIDLow(), creature->GetEntry(), creature->GetPositionX(), creature->GetPositionY());
        delete creature;
        return NULL;
    }

    if (!map->AddToMap(creature))
    {
        sLog->outError("Transport::CreateNPCPassenger: Creature (guidlow %u, entry %u) not added. Something went wrong in AddToMap)",
                       creature->GetGUIDLow(), creature->GetEntry());
        delete creature;
        return NULL;
    }

    _staticPassengers.insert(creature);
    sScriptMgr->OnAddCreaturePassenger(this, creature);
    return creature;
}

GameObject* Transport::CreateGOPassenger(uint32 guid, GameObjectData const* data)
{
    sLog->outDebug(LOG_FILTER_TRANSPORTS, "Transport::CreateGOPassenger: Create GO lowguid %u", guid);

    Map* map = GetMap();
    GameObject* go = new GameObject();

    if (!go->LoadGameObjectFromDB(guid, map, false))
    {
        sLog->outError("Transport::CreateGOPassenger: GameObject lowguid %u not loaded. Something went wrong in LoadGameObjectFromDB)", guid);
        delete go;
        return NULL;
    }

    float x = data->posX;
    float y = data->posY;
    float z = data->posZ;
    float o = data->orientation;

    go->SetTransport(this);
    go->m_movementInfo.transport.guid = GetGUID();
    go->m_movementInfo.transport.pos.Relocate(x, y, z, o);
    CalculatePassengerPosition(x, y, z, o);
    go->Relocate(x, y, z, o);

    if (!go->IsPositionValid())
    {
        sLog->outError("Transport::CreateGOPassenger: GameObject (guidlow %u, entry %u) not created. Suggested coordinates aren't valid (X: %f Y: %f)",
                       go->GetGUIDLow(), go->GetEntry(), go->GetPositionX(), go->GetPositionY());
        delete go;
        return NULL;
    }

    if (!map->AddToMap(go))
    {
        sLog->outError("Transport::CreateGOPassenger: GameObject (guidlow %u, entry %u) not added. Something went wrong in AddToMap)",
                       go->GetGUIDLow(), go->GetEntry());
        delete go;
        return NULL;
    }

    _staticPassengers.insert(go);
    return go;
}

TempSummon* Transport::SummonPassenger(uint32 entry, Position const& pos, TempSummonType summonType, SummonPropertiesEntry const* properties /*= NULL*/, uint32 duration /*= 0*/, Unit* summoner /*= NULL*/, uint32 spellId /*= 0*/, uint32 vehId /*= 0*/)
{
    sLog->outDebug(LOG_FILTER_TRANSPORTS, "Transport::SummonPassenger: Summon creature entry %u (X: %f Y: %f Z: %f) summonType: %u duration: %u on map %u",
                   entry, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), summonType, duration, GetMapId());

    Map* map = FindMap();
    if (!map)
    {
        sLog->outError("Transport::SummonPassenger: Couldn't find map %u", GetMapId());
        return NULL;
    }

    uint32 mask = UNIT_MASK_SUMMON;
    if (properties)
    {
        switch (properties->Category)
        {
            case SUMMON_CATEGORY_PET:
                mask = UNIT_MASK_GUARDIAN;
                break;
            case SUMMON_CATEGORY_PUPPET:
                mask = UNIT_MASK_PUPPET;
                break;
            case SUMMON_CATEGORY_VEHICLE:
                mask = UNIT_MASK_MINION;
                break;
            case SUMMON_CATEGORY_WILD:
            case SUMMON_CATEGORY_ALLY:
            case SUMMON_CATEGORY_UNK:
            {
                switch (properties->Type)
                {
                    case SUMMON_TYPE_MINION:
                    case SUMMON_TYPE_GUARDIAN:
                    case SUMMON_TYPE_GUARDIAN2:
                        mask = UNIT_MASK_GUARDIAN;
                        break;
                    case SUMMON_TYPE_TOTEM:
                    case SUMMON_TYPE_OBJECT:
                        mask = UNIT_MASK_TOTEM;
                        break;
                    case SUMMON_TYPE_VEHICLE:
                    case SUMMON_TYPE_VEHICLE2:
                        mask = UNIT_MASK_SUMMON;
                        break;
                    case SUMMON_TYPE_MINIPET:
                        mask = UNIT_MASK_MINION;
                        break;
                    default:
                        if (properties->Flags & 512) // Mirror Image, Summon Gargoyle
                            mask = UNIT_MASK_GUARDIAN;
                        break;
                }
                break;
            }
            default:
                return NULL;
        }
    }

    uint32 phase = PHASEMASK_NORMAL;
    if (summoner)
        phase = summoner->GetPhaseMask();

    TempSummon* summon = NULL;
    switch (mask)
    {
        case UNIT_MASK_SUMMON:
            summon = new TempSummon(properties, summoner, false);
            break;
        case UNIT_MASK_GUARDIAN:
            summon = new Guardian(properties, summoner, false);
            break;
        case UNIT_MASK_PUPPET:
            summon = new Puppet(properties, summoner);
            break;
        case UNIT_MASK_TOTEM:
            summon = new Totem(properties, summoner);
            break;
        case UNIT_MASK_MINION:
            summon = new Minion(properties, summoner, false);
            break;
    }

    float x, y, z, o;
    pos.GetPosition(x, y, z, o);
    CalculatePassengerPosition(x, y, z, o);

    if (!summon->Create(sObjectMgr->GenerateLowGuid(HIGHGUID_UNIT), map, phase, entry, vehId, TEAM_OTHER, x, y, z, o, NULL))
    {
        sLog->outError("Transport::SummonPassenger: Creature entry %u not created. Something went wrong in Create)", entry);
        delete summon;
        return NULL;
    }

    summon->SetUInt32Value(UNIT_CREATED_BY_SPELL, spellId);

    summon->SetTransport(this);
    summon->AddUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
    summon->m_movementInfo.transport.guid = GetGUID();
    summon->m_movementInfo.transport.pos.Relocate(pos);
    summon->Relocate(x, y, z, o);
    summon->SetHomePosition(x, y, z, o);
    summon->SetTransportHomePosition(pos);

    /// @HACK - transport models are not added to map's dynamic LoS calculations
    ///         because the current GameObjectModel cannot be moved without recreating - no mmaps
    // summon->AddUnitState(UNIT_STATE_IGNORE_PATHFINDING);

    summon->InitStats(duration);

    if (!map->AddToMap<Creature>(summon))
    {
        sLog->outError("Transport::SummonPassenger: Creature (guidlow %d, entry %d) not added. Something went wrong in AddToMap)", summon->GetGUIDLow(), summon->GetEntry());
        delete summon;
        return NULL;
    }

    _staticPassengers.insert(summon);

    summon->InitSummon();
    summon->SetTempSummonType(summonType);

    return summon;
}

void Transport::UpdatePosition(float x, float y, float z, float o)
{
    bool newActive = GetMap()->IsGridLoaded(x, y);
    Cell oldCell(GetPositionX(), GetPositionY());

    Relocate(x, y, z, o);
    UpdateModelPosition();

    UpdatePassengerPositions(_passengers);

    /* There are four possible scenarios that trigger loading/unloading passengers:
    1. transport moves from inactive to active grid
    2. the grid that transport is currently in becomes active
    3. transport moves from active to inactive grid
    4. the grid that transport is currently in unloads
    */
    if (_staticPassengers.empty() && newActive) // 1.
        LoadStaticPassengers();
    else if (!_staticPassengers.empty() && !newActive && oldCell.DiffGrid(Cell(GetPositionX(), GetPositionY()))) // 3.
        UnloadStaticPassengers();
    else
        UpdatePassengerPositions(_staticPassengers);
    // 4. is handed by grid unload
}

void Transport::LoadStaticPassengers()
{
    if (uint32 mapId = GetGOInfo()->moTransport.mapID)
    {
        CellObjectGuidsMap const& cells = sObjectMgr->GetMapObjectGuids(mapId, GetMap()->GetSpawnMode());
        CellGuidSet::const_iterator guidEnd;
        for (CellObjectGuidsMap::const_iterator cellItr = cells.begin(); cellItr != cells.end(); ++cellItr)
        {
            // Creatures on transport
            guidEnd = cellItr->second.creatures.end();
            for (CellGuidSet::const_iterator guidItr = cellItr->second.creatures.begin(); guidItr != guidEnd; ++guidItr)
                CreateNPCPassenger(*guidItr, sObjectMgr->GetCreatureData(*guidItr));

            // GameObjects on transport
            guidEnd = cellItr->second.gameobjects.end();
            for (CellGuidSet::const_iterator guidItr = cellItr->second.gameobjects.begin(); guidItr != guidEnd; ++guidItr)
                CreateGOPassenger(*guidItr, sObjectMgr->GetGOData(*guidItr));
        }
    }
}

void Transport::UnloadStaticPassengers()
{
    while (!_staticPassengers.empty())
    {
        WorldObject* obj = *_staticPassengers.begin();
        obj->AddObjectToRemoveList();   // also removes from _staticPassengers
    }
}

void Transport::EnableMovement(bool enabled)
{
    if (!GetGOInfo()->moTransport.canBeStopped)
        return;

    _pendingStop = !enabled;
}

void Transport::EnablePassengerMovement(PassengerSet& passengers, bool enabled)
{
    if (passengers.empty())
        return;

    for (PassengerSet::const_iterator itr = passengers.begin(); itr != passengers.end(); ++itr)
    {
        WorldObject* passenger = *itr;

        switch (passenger->GetTypeId())
        {
            case TYPEID_UNIT:
            {
                Creature* pCreature = passenger->ToCreature();

                if (enabled)
                    pCreature->AddUnitState(UNIT_STATE_MOVING);
                else
                    pCreature->ClearUnitState(UNIT_STATE_MOVING);
                pCreature->SendMovementFlagUpdate();
            }break;

            default:
                break;
        }
    }
}

void Transport::MoveToNextWaypoint()
{
    // Set frames
    _currentFrame = _nextFrame++;
    if (_nextFrame == GetKeyFrames().end())
        _nextFrame = GetKeyFrames().begin();
}

float Transport::CalculateSegmentPos(float now)
{
    KeyFrame const& frame = *_currentFrame;
    const float speed = float(m_goInfo->moTransport.moveSpeed);
    const float accel = float(m_goInfo->moTransport.accelRate / 10.0f);
    float timeSinceStop = frame.TimeFrom + (now - (1.0f / IN_MILLISECONDS) * frame.DepartureTime);
    float timeUntilStop = frame.TimeTo - (now - (1.0f / IN_MILLISECONDS) * frame.DepartureTime);
    float segmentPos, dist;
    float accelTime = _transportInfo->accelTime;
    float accelDist = _transportInfo->accelDist;

    // calculate from nearest stop, less confusing calculation...
    if (timeSinceStop < timeUntilStop)
    {
        if (timeSinceStop < accelTime)
            dist = 0.5f * accel * timeSinceStop * timeSinceStop;
        else
            dist = accelDist + (timeSinceStop - accelTime) * speed;
        segmentPos = dist - frame.DistSinceStop;
    }
    else
    {
        if (timeUntilStop < _transportInfo->accelTime)
            dist = 0.5f * accel * timeUntilStop * timeUntilStop;
        else
            dist = accelDist + (timeUntilStop - accelTime) * speed;
        segmentPos = frame.DistUntilStop - dist;
    }

    return segmentPos / frame.NextDistFromPrev;
}

bool Transport::TeleportTransport(uint32 newMapid, float x, float y, float z, float o)
{
    Map const* oldMap = GetMap();

    if (oldMap->GetId() != newMapid)
    {
        Map* newMap = sMapMgr->CreateBaseMap(newMapid);
        UnloadStaticPassengers();
        GetMap()->RemoveFromMap<Transport>(this, false);
        SetMap(newMap);

        for (_passengerTeleportItr = _passengers.begin(); _passengerTeleportItr != _passengers.end();)
        {
            WorldObject* obj = (*_passengerTeleportItr++);

            float destX, destY, destZ, destO;
            obj->m_movementInfo.transport.pos.GetPosition(destX, destY, destZ, destO);
            TransportBase::CalculatePassengerPosition(destX, destY, destZ, destO, x, y, z, o);

            switch (obj->GetTypeId())
            {
                case TYPEID_PLAYER:
                    if (obj->IsInWorld())
                        if (obj->ToPlayer()->TeleportTo(newMapid, destX, destY, destZ, destO, TELE_TO_NOT_LEAVE_TRANSPORT))
                            break;
                    RemovePassenger(obj);
                    break;
                case TYPEID_DYNAMICOBJECT:
                    obj->AddObjectToRemoveList();
                    break;
                default:
                    RemovePassenger(obj);
                    break;
            }
        }

        Relocate(x, y, z, o);
        GetMap()->AddToMap<Transport>(this);
        return true;
    }
    else
    {
        // Teleport players, they need to know it
        for (PassengerSet::iterator itr = _passengers.begin(); itr != _passengers.end(); ++itr)
        {
            if ((*itr)->GetTypeId() == TYPEID_PLAYER)
            {
                float destX, destY, destZ, destO;
                (*itr)->m_movementInfo.transport.pos.GetPosition(destX, destY, destZ, destO);
                TransportBase::CalculatePassengerPosition(destX, destY, destZ, destO, x, y, z, o);

                (*itr)->ToUnit()->NearTeleportTo(destX, destY, destZ, destO);
            }
        }

        UpdatePosition(x, y, z, o);
        return false;
    }
}

void Transport::UpdatePassengerPositions(PassengerSet& passengers)
{
    for (PassengerSet::iterator itr = passengers.begin(); itr != passengers.end(); ++itr)
    {
        WorldObject* passenger = *itr;

        if (!passenger->IsInWorld())
            continue;

        // transport teleported but passenger not yet (can happen for players)
        if (passenger->GetMap() != GetMap())
            continue;

        // if passenger is on vehicle we have to assume the vehicle is also on transport
        // and its the vehicle that will be updating its passengers
        if (Unit* unit = passenger->ToUnit())
            if (unit->GetVehicle())
                continue;

        // Do not use Unit::UpdatePosition here, we don't want to remove auras
        // as if regular movement occurred
        float x, y, z, o;
        passenger->m_movementInfo.transport.pos.GetPosition(x, y, z, o);
        CalculatePassengerPosition(x, y, z, o);
        switch (passenger->GetTypeId())
        {
            case TYPEID_UNIT:
            {
                Creature* creature = passenger->ToCreature();
                GetMap()->CreatureRelocation(creature, x, y, z, o, false);
                creature->GetTransportHomePosition(x, y, z, o);
                CalculatePassengerPosition(x, y, z, o);
                creature->SetHomePosition(x, y, z, o);
                break;
            }
            case TYPEID_PLAYER:
                if (passenger->IsInWorld())
                    GetMap()->PlayerRelocation(passenger->ToPlayer(), x, y, z, o);
                break;
            case TYPEID_GAMEOBJECT:
                GetMap()->GameObjectRelocation(passenger->ToGameObject(), x, y, z, o, false);
                break;
            case TYPEID_DYNAMICOBJECT:
                GetMap()->DynamicObjectRelocation(passenger->ToDynObject(), x, y, z, o);
                break;
            default:
                break;
        }

        if (Unit* unit = passenger->ToUnit())
            if (Vehicle* vehicle = unit->GetVehicleKit())
                vehicle->RelocatePassengers(x, y, z, o);
    }
}

void Transport::DoEventIfAny(KeyFrame const& node, bool departure)
{
    if (uint32 eventid = departure ? node.Node->departureEventID : node.Node->arrivalEventID)
    {
        sLog->outDebug(LOG_FILTER_TRANSPORTS, "Taxi %s event %u of node %u of %s path", departure ? "departure" : "arrival", eventid, node.Node->index, GetName());
        GetMap()->ScriptsStart(sEventScripts, eventid, this, this);
        EventInform(eventid);
    }
}

void Transport::SetMoving(bool val)
{
    _isMoving = val;

    EnablePassengerMovement(_staticPassengers, val);
    EnablePassengerMovement(_passengers, val);
}

void Transport::BuildUpdate(UpdateDataMapType& data_map)
{
    Map::PlayerList const& players = GetMap()->GetPlayers();
    if (players.isEmpty())
        return;

    for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        BuildFieldsUpdate(itr->getSource(), data_map);

    ClearUpdateMask(true);
}