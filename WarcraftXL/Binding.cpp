#include "WarcraftXL/Binding.h"

#include <cstddef>
#include <cstdint>

#include "Engine/Source/Base/CDataStore.h"
#include "Engine/Source/BlizzardCore/SFile2-Core.h"
#include "Engine/Source/FrameScript/FrameScript.h"
#include "Engine/Source/Gx/CGxDevice/CGxDevice.h"
#include "Engine/Source/Gx/Gx.h"
#include "Engine/Source/Tempest/CRect.h"
#include "Engine/Source/Tempest/C3Vector.h"
#include "Engine/Source/lua-5.1/src/lua.h"
#include "WoW/Source/Object/Container.h"
#include "WoW/Source/Object/Corpse.h"
#include "WoW/Source/Object/DynamicObject.h"
#include "WoW/Source/Object/GameObject.h"
#include "WoW/Source/Object/Item.h"
#include "WoW/Source/Object/Object.h"
#include "WoW/Source/Object/ObjectClient/Container_C.h"
#include "WoW/Source/Object/ObjectClient/Corpse_C.h"
#include "WoW/Source/Object/ObjectClient/DynamicObject_C.h"
#include "WoW/Source/Object/ObjectClient/GameObject_C.h"
#include "WoW/Source/Object/ObjectClient/Item_C.h"
#include "WoW/Source/Object/ObjectClient/Object_C.h"
#include "WoW/Source/Object/ObjectClient/Player_C.h"
#include "WoW/Source/Object/ObjectClient/Types.h"
#include "WoW/Source/Object/ObjectClient/Unit_C.h"
#include "WoW/Source/Object/ObjectClient/VisibleItem_C.h"
#include "WoW/Source/Object/Player.h"
#include "WoW/Source/Object/Unit.h"
#include "WoW/Source/ObjectMgrClient/ObjectMgrClient.h"
#include "WoW/Source/Ui/GameUI.h"
#include "WoW/Source/WorldClient/MapArea.h"
#include "WoW/Source/WorldClient/World.h"
#include "wxl/events.h"
#include "wxl/services/script.h"

// The contract numbers object types exactly as the client does, so a type id crosses as is.
static_assert(ID_OBJECT == WXL_OBJECT_UNKNOWN && ID_ITEM == WXL_OBJECT_ITEM && ID_CONTAINER == WXL_OBJECT_CONTAINER
                  && ID_UNIT == WXL_OBJECT_UNIT && ID_PLAYER == WXL_OBJECT_PLAYER && ID_GAMEOBJECT == WXL_OBJECT_GAMEOBJECT
                  && ID_DYNAMICOBJECT == WXL_OBJECT_DYNAMIC && ID_CORPSE == WXL_OBJECT_CORPSE,
              "wxl_object_type numbers the client's OBJECT_TYPE_ID");

namespace {
    // The runtime hands back the CGObject_C* a seam gave it, and asks a kind's rows only for an
    // object of that kind, so each cast below is the one the client itself would make.
    const CGObject_C* Object(const void* object) {
        return static_cast<const CGObject_C*>(object);
    }

    const CGUnit_C* Unit(const void* object) {
        return static_cast<const CGUnit_C*>(Object(object));
    }

    const CGPlayer_C* Player(const void* object) {
        return static_cast<const CGPlayer_C*>(Object(object));
    }

    const CGItem_C* Item(const void* object) {
        return static_cast<const CGItem_C*>(Object(object));
    }

    const CGContainer_C* Container(const void* object) {
        return static_cast<const CGContainer_C*>(Object(object));
    }

    const CGGameObject_C* GameObject(const void* object) {
        return static_cast<const CGGameObject_C*>(Object(object));
    }

    const CGDynamicObject_C* DynamicObject(const void* object) {
        return static_cast<const CGDynamicObject_C*>(Object(object));
    }

    const CGCorpse_C* Corpse(const void* object) {
        return static_cast<const CGCorpse_C*>(Object(object));
    }

    template <class T, size_t N>
    constexpr uint32_t Extent(const T (&)[N]) {
        return static_cast<uint32_t>(N);
    }

    wxl_bool Give(wxl_vec3* out, const C3Vector& v) {
        out->x = v.x;
        out->y = v.y;
        out->z = v.z;
        return WXL_TRUE;
    }

    lua_State* State(void* state) {
        return static_cast<lua_State*>(state);
    }
}

namespace WarcraftXL::Binding {
    // --- world.def ------------------------------------------------------------------------------

    int32_t WXL_CALL map_id(void) {
        return World::QueryMapId();
    }

    // --- objectmgr.def --------------------------------------------------------------------------

    const void* WXL_CALL local_player_object(void) {
        return ClntObjMgrGetActivePlayerObj();
    }

    // --- object.def -----------------------------------------------------------------------------

    uint64_t WXL_CALL object_guid(const void* object) {
        return Object(object)->m_obj->m_guid.m_guid;
    }

    uint32_t WXL_CALL object_entry(const void* object) {
        return Object(object)->m_obj->m_entryID;
    }

    float WXL_CALL object_scale(const void* object) {
        return Object(object)->m_obj->m_scale;
    }

    uint32_t WXL_CALL object_type_id(const void* object) {
        return static_cast<uint32_t>(Object(object)->m_typeId);
    }

    wxl_bool WXL_CALL object_position(const void* object, wxl_vec3* out) {
        const CGObject_C* o = Object(object);
        switch (o->m_typeId) {
            case ID_UNIT:
            case ID_PLAYER:
            case ID_GAMEOBJECT:
            case ID_DYNAMICOBJECT:
                return Give(out, o->GetPosition());
            case ID_CORPSE:
                // The fork declares no CGCorpse_C::GetPosition; the corpse's CPassenger holds it.
                return Give(out, Corpse(object)->m_passenger.m_position);
            default:
                return WXL_FALSE;
        }
    }

    wxl_bool WXL_CALL object_facing(const void* object, float* out) {
        const CGObject_C* o = Object(object);
        switch (o->m_typeId) {
            case ID_UNIT:
            case ID_PLAYER:
            case ID_GAMEOBJECT:
            case ID_DYNAMICOBJECT:
            case ID_CORPSE:
                *out = o->GetFacing();
                return WXL_TRUE;
            default:
                return WXL_FALSE;
        }
    }

    const char* WXL_CALL object_name(const void* object) {
        const CGObject_C* o = Object(object);
        switch (o->m_typeId) {
            case ID_UNIT:
            case ID_PLAYER:
            case ID_GAMEOBJECT:
            case ID_ITEM:
            case ID_CONTAINER:
                return o->GetObjectName();
            default:
                return nullptr;
        }
    }

    // --- unit.def -------------------------------------------------------------------------------

    uint32_t WXL_CALL unit_health(const void* object) {
        return static_cast<uint32_t>(Unit(object)->m_unit->m_unitFieldHealth);
    }

    uint32_t WXL_CALL unit_max_health(const void* object) {
        return Unit(object)->m_unit->m_unitFieldMaxhealth;
    }

    uint32_t WXL_CALL unit_level(const void* object) {
        return Unit(object)->m_unit->m_unitFieldLevel;
    }

    uint64_t WXL_CALL unit_target(const void* object) {
        return Unit(object)->m_unit->m_unitFieldTarget.m_guid;
    }

    wxl_bool WXL_CALL unit_power(const void* object, uint32_t index, uint32_t* out) {
        const CGUnitData* unit = Unit(object)->m_unit;
        if (index >= Extent(unit->m_unitFieldPower)) {
            return WXL_FALSE;
        }
        *out = unit->m_unitFieldPower[index];
        return WXL_TRUE;
    }

    wxl_bool WXL_CALL unit_max_power(const void* object, uint32_t index, uint32_t* out) {
        const CGUnitData* unit = Unit(object)->m_unit;
        if (index >= Extent(unit->m_unitFieldMaxpower)) {
            return WXL_FALSE;
        }
        *out = unit->m_unitFieldMaxpower[index];
        return WXL_TRUE;
    }

    uint32_t WXL_CALL unit_bytes0(const void* object) {
        const uint8_t* bytes = Unit(object)->m_unit->m_unitFieldBytes0;
        return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8)
            | (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
    }

    uint32_t WXL_CALL unit_display_id(const void* object) {
        return Unit(object)->m_unit->m_unitFieldDisplayid;
    }

    uint32_t WXL_CALL unit_native_display_id(const void* object) {
        return Unit(object)->m_unit->m_unitFieldNativedisplayid;
    }

    uint32_t WXL_CALL unit_mount_display_id(const void* object) {
        return Unit(object)->m_unit->m_unitFieldMountdisplayid;
    }

    uint32_t WXL_CALL unit_faction_template(const void* object) {
        return Unit(object)->m_unit->m_unitFieldFactiontemplate;
    }

    uint32_t WXL_CALL unit_flags(const void* object) {
        return Unit(object)->m_unit->m_unitFieldFlags;
    }

    uint32_t WXL_CALL unit_flags2(const void* object) {
        return Unit(object)->m_unit->m_unitFieldFlags2;
    }

    uint32_t WXL_CALL unit_dynamic_flags(const void* object) {
        return Unit(object)->m_unit->m_unitDynamicFlags;
    }

    uint32_t WXL_CALL unit_npc_flags(const void* object) {
        return Unit(object)->m_unit->m_unitNpcFlags;
    }

    float WXL_CALL unit_bounding_radius(const void* object) {
        return Unit(object)->m_unit->m_unitFieldBoundingradius;
    }

    float WXL_CALL unit_combat_reach(const void* object) {
        return Unit(object)->m_unit->m_unitFieldCombatreach;
    }

    uint64_t WXL_CALL unit_charm(const void* object) {
        return Unit(object)->m_unit->m_unitFieldCharm.m_guid;
    }

    uint64_t WXL_CALL unit_summon(const void* object) {
        return Unit(object)->m_unit->m_unitFieldSummon.m_guid;
    }

    uint64_t WXL_CALL unit_charmed_by(const void* object) {
        return Unit(object)->m_unit->m_unitFieldCharmedby.m_guid;
    }

    uint64_t WXL_CALL unit_summoned_by(const void* object) {
        return Unit(object)->m_unit->m_unitFieldSummonedby.m_guid;
    }

    uint64_t WXL_CALL unit_created_by(const void* object) {
        return Unit(object)->m_unit->m_unitFieldCreatedby.m_guid;
    }

    // --- player.def -----------------------------------------------------------------------------

    uint32_t WXL_CALL player_flags(const void* object) {
        return Player(object)->m_player->m_playerFlags;
    }

    uint32_t WXL_CALL player_guild_id(const void* object) {
        return Player(object)->m_player->m_playerGuildid;
    }

    uint32_t WXL_CALL player_guild_rank(const void* object) {
        return Player(object)->m_player->m_playerGuildrank;
    }

    uint32_t WXL_CALL player_chosen_title(const void* object) {
        return Player(object)->m_player->m_playerChosenTitle;
    }

    wxl_bool WXL_CALL player_visible_item_id(const void* object, uint32_t index, int32_t* out) {
        const CGPlayerData* player = Player(object)->m_player;
        if (index >= Extent(player->m_playerVisibleItems)) {
            return WXL_FALSE;
        }
        *out = player->m_playerVisibleItems[index].m_itemId;
        return WXL_TRUE;
    }

    int32_t WXL_CALL visible_item_id(const void* object) {
        return static_cast<const VisibleItem_C*>(object)->m_itemId;
    }

    // --- item.def -------------------------------------------------------------------------------

    uint64_t WXL_CALL item_owner(const void* object) {
        return Item(object)->m_item->m_itemFieldOwner.m_guid;
    }

    uint64_t WXL_CALL item_contained_in(const void* object) {
        return Item(object)->m_item->m_itemFieldContained.m_guid;
    }

    uint64_t WXL_CALL item_creator(const void* object) {
        return Item(object)->m_item->m_itemFieldCreator.m_guid;
    }

    uint32_t WXL_CALL item_stack_count(const void* object) {
        return static_cast<uint32_t>(Item(object)->m_item->m_itemFieldStackCount);
    }

    uint32_t WXL_CALL item_duration(const void* object) {
        return Item(object)->m_item->m_itemFieldDuration;
    }

    uint32_t WXL_CALL item_flags(const void* object) {
        return Item(object)->m_item->m_itemFieldFlags;
    }

    uint32_t WXL_CALL item_durability(const void* object) {
        return Item(object)->m_item->m_itemFieldDurability;
    }

    uint32_t WXL_CALL item_max_durability(const void* object) {
        return Item(object)->m_item->m_itemFieldMaxdurability;
    }

    // --- container.def --------------------------------------------------------------------------

    uint32_t WXL_CALL container_num_slots(const void* object) {
        return Container(object)->m_container->m_containerFieldNumSlots;
    }

    wxl_bool WXL_CALL container_slot(const void* object, uint32_t index, uint64_t* out) {
        const CGContainerData* container = Container(object)->m_container;
        if (index >= Extent(container->m_containerFieldSlot1)) {
            return WXL_FALSE;
        }
        *out = container->m_containerFieldSlot1[index].m_guid;
        return WXL_TRUE;
    }

    // --- gameobject.def -------------------------------------------------------------------------

    uint64_t WXL_CALL gameobject_created_by(const void* object) {
        return GameObject(object)->m_gameObject->m_createdBy.m_guid;
    }

    uint32_t WXL_CALL gameobject_display_id(const void* object) {
        return GameObject(object)->m_gameObject->m_displayID;
    }

    uint32_t WXL_CALL gameobject_flags(const void* object) {
        return GameObject(object)->m_gameObject->m_flags;
    }

    uint16_t WXL_CALL gameobject_dynamic_flags(const void* object) {
        return GameObject(object)->m_gameObject->m_dynamicFlags;
    }

    uint32_t WXL_CALL gameobject_faction(const void* object) {
        return GameObject(object)->m_gameObject->m_faction;
    }

    uint32_t WXL_CALL gameobject_level(const void* object) {
        return GameObject(object)->m_gameObject->m_level;
    }

    int8_t WXL_CALL gameobject_state(const void* object) {
        return GameObject(object)->m_gameObject->m_state;
    }

    int8_t WXL_CALL gameobject_type_id(const void* object) {
        return GameObject(object)->m_gameObject->m_typeID;
    }

    int8_t WXL_CALL gameobject_art_kit(const void* object) {
        return GameObject(object)->m_gameObject->m_artKit;
    }

    // --- dynamicobject.def ----------------------------------------------------------------------

    uint64_t WXL_CALL dynamic_caster(const void* object) {
        return DynamicObject(object)->m_dynamicObject->m_caster.m_guid;
    }

    uint32_t WXL_CALL dynamic_spell_id(const void* object) {
        return DynamicObject(object)->m_dynamicObject->m_spellID;
    }

    float WXL_CALL dynamic_radius(const void* object) {
        return DynamicObject(object)->m_dynamicObject->m_radius;
    }

    uint32_t WXL_CALL dynamic_cast_time(const void* object) {
        return DynamicObject(object)->m_dynamicObject->m_castTime;
    }

    // --- corpse.def -----------------------------------------------------------------------------

    uint64_t WXL_CALL corpse_owner(const void* object) {
        return Corpse(object)->m_corpse->m_corpseFieldOwner.m_guid;
    }

    uint64_t WXL_CALL corpse_party(const void* object) {
        return Corpse(object)->m_corpse->m_corpseFieldParty.m_guid;
    }

    uint32_t WXL_CALL corpse_display_id(const void* object) {
        return Corpse(object)->m_corpse->m_corpseFieldDisplayId;
    }

    uint32_t WXL_CALL corpse_guild(const void* object) {
        return Corpse(object)->m_corpse->m_corpseFieldGuild;
    }

    uint32_t WXL_CALL corpse_flags(const void* object) {
        return Corpse(object)->m_corpse->m_corpseFieldFlags;
    }

    uint32_t WXL_CALL corpse_dynamic_flags(const void* object) {
        return Corpse(object)->m_corpse->m_corpseFieldDynamicFlags;
    }

    // --- gameui.def -----------------------------------------------------------------------------

    uint64_t WXL_CALL ui_target_guid(void) {
        return CGGameUI::m_lockedTarget.m_guid;
    }

    int32_t WXL_CALL ui_initialized(void) {
        return CGGameUI::m_initialized;
    }

    uint8_t WXL_CALL ui_logging_in(void) {
        return CGGameUI::m_loggingIn;
    }

    // --- maparea.def ----------------------------------------------------------------------------

    int32_t WXL_CALL map_area_tile_x(const void* object) {
        return static_cast<const CMapArea*>(object)->m_index.x;
    }

    int32_t WXL_CALL map_area_tile_y(const void* object) {
        return static_cast<const CMapArea*>(object)->m_index.y;
    }

    // --- script.def -----------------------------------------------------------------------------

    void* WXL_CALL script_state(void) {
        return FrameScript_GetContext();
    }

    wxl_bool WXL_CALL script_global_is_set(void* state, const char* name) {
        lua_State* L = State(state);
        lua_getfield(L, LUA_GLOBALSINDEX, name);
        const int type = lua_type(L, -1);
        lua_pop(L, 1);
        return type > LUA_TNIL ? WXL_TRUE : WXL_FALSE;
    }

    void WXL_CALL script_set_global_closure(void* state, const char* name, wxl_script_native_fn fn, uint32_t slot) {
        lua_State* L = State(state);
        lua_pushnumber(L, static_cast<lua_Number>(slot));
        lua_pushcclosure(L, reinterpret_cast<lua_CFunction>(fn), 1);
        lua_setfield(L, LUA_GLOBALSINDEX, name);
    }

    void WXL_CALL script_clear_global(void* state, const char* name) {
        lua_State* L = State(state);
        lua_pushnil(L);
        lua_setfield(L, LUA_GLOBALSINDEX, name);
    }

    uint32_t WXL_CALL script_closure_slot(void* state) {
        return static_cast<uint32_t>(lua_tonumber(State(state), lua_upvalueindex(1)));
    }

    int32_t WXL_CALL script_top(void* state) {
        return lua_gettop(State(state));
    }

    uint32_t WXL_CALL script_value_type(void* state, int32_t index) {
        switch (lua_type(State(state), index)) {
            case LUA_TNUMBER:
                return WXL_VALUE_NUMBER;
            case LUA_TSTRING:
                return WXL_VALUE_STRING;
            case LUA_TBOOLEAN:
                return WXL_VALUE_BOOL;
            default:
                return WXL_VALUE_NIL;
        }
    }

    double WXL_CALL script_to_number(void* state, int32_t index) {
        return lua_tonumber(State(state), index);
    }

    const char* WXL_CALL script_to_string(void* state, int32_t index, uint32_t* length) {
        size_t size = 0;
        const char* text = lua_tolstring(State(state), index, &size);
        if (length) {
            *length = static_cast<uint32_t>(size);
        }
        return text;
    }

    wxl_bool WXL_CALL script_to_bool(void* state, int32_t index) {
        return lua_toboolean(State(state), index) ? WXL_TRUE : WXL_FALSE;
    }

    void WXL_CALL script_push_number(void* state, double value) {
        lua_pushnumber(State(state), value);
    }

    void WXL_CALL script_push_string(void* state, const char* text, uint32_t length) {
        lua_pushlstring(State(state), text ? text : "", text ? length : 0u);
    }

    void WXL_CALL script_push_bool(void* state, wxl_bool value) {
        lua_pushboolean(State(state), value ? 1 : 0);
    }

    void WXL_CALL script_push_nil(void* state) {
        lua_pushnil(State(state));
    }

    void WXL_CALL script_pop(void* state, int32_t count) {
        if (count > 0) {
            lua_pop(State(state), count);
        }
    }

    void WXL_CALL script_error(void* state, const char* message) {
        lua_State* L = State(state);
        lua_pushstring(L, message ? message : "WarcraftXL: error");
        lua_error(L);
    }

    void WXL_CALL script_run(const char* source, const char* name) {
        FrameScript_Execute(source, name, nullptr);
    }

    void WXL_CALL script_create_events(const char** names, uint32_t count) {
        FrameScript_CreateEvents(names, count);
    }

    void WXL_CALL script_signal_event(uint32_t index, void* state, int32_t arg_count) {
        FrameScript_SignalEvent(index, State(state), arg_count);
    }

    // The NetClient rows read the message a seam was handed, and never move its read cursor:
    // that cursor is where the client's own parsing starts.
    const void* WXL_CALL message_data(const void* message) {
        return message ? static_cast<const CDataStore*>(message)->m_data : nullptr;
    }

    uint32_t WXL_CALL message_size(const void* message) {
        return message ? static_cast<const CDataStore*>(message)->m_size : 0u;
    }

    uint32_t WXL_CALL message_read(const void* message) {
        return message ? static_cast<const CDataStore*>(message)->m_read : 0u;
    }

    // The Storage rows are the client's own file layer, called where it lives. A `file` is the
    // SFileImpl the client handed back; nothing above this knows what it is.
    SFileImpl* File(void* file) {
        return static_cast<SFileImpl*>(file);
    }

    wxl_bool WXL_CALL file_exists(const char* path) {
        return path && SFile::FileExists(path) ? WXL_TRUE : WXL_FALSE;
    }

    void* WXL_CALL file_open(const char* path) {
        SFileImpl* file = nullptr;
        return path && SFileOpenFile(path, &file) ? file : nullptr;
    }

    uint64_t WXL_CALL file_size(void* file) {
        if (!file) {
            return 0;
        }
        uint32_t high = 0;
        uint32_t low = SFileGetFileSize(File(file), &high);
        return (static_cast<uint64_t>(high) << 32) | low;
    }

    wxl_bool WXL_CALL file_seek(void* file, uint64_t offset) {
        if (!file) {
            return WXL_FALSE;
        }
        int32_t high = static_cast<int32_t>(offset >> 32);
        // Method 0 is FILE_BEGIN: the offset is absolute, which is what read_at means.
        return SFile::SetFilePointer(File(file), static_cast<int32_t>(offset & 0xFFFFFFFFu), &high, 0)
                   ? WXL_TRUE
                   : WXL_FALSE;
    }

    uint32_t WXL_CALL file_read(void* file, void* buffer, uint32_t count) {
        if (!file || !buffer || !count) {
            return 0;
        }
        uint32_t read = 0;
        SFileReadFile(File(file), buffer, count, &read, nullptr, 0);
        return read;
    }

    void WXL_CALL file_close(void* file) {
        if (file) {
            SFileCloseFile(File(file));
        }
    }

    wxl_bool WXL_CALL archive_mount(const char* path, int32_t priority) {
        SArchive* archive = nullptr;
        return path && SFile::OpenArchive(path, priority, 0, &archive) ? WXL_TRUE : WXL_FALSE;
    }

    // --- Gx ---------------------------------------------------------------------------------
    // GxHasContext and GxIsWindowVisible test the device pointer themselves.
    // GxCapsWindowSizeInScreenCoords does not, so the size answers false without a device.

    wxl_bool WXL_CALL gx_device_exists() {
        return g_theGxDevicePtr ? WXL_TRUE : WXL_FALSE;
    }

    wxl_bool WXL_CALL gx_has_context() {
        return GxHasContext() ? WXL_TRUE : WXL_FALSE;
    }

    wxl_bool WXL_CALL gx_window_visible() {
        return GxIsWindowVisible() ? WXL_TRUE : WXL_FALSE;
    }

    wxl_bool WXL_CALL gx_window_size(uint32_t* width, uint32_t* height) {
        if (!width || !height || !g_theGxDevicePtr) {
            return WXL_FALSE;
        }
        CRect rect = {};
        GxCapsWindowSizeInScreenCoords(rect);
        const float w = rect.maxX - rect.minX;
        const float h = rect.maxY - rect.minY;
        *width = w > 0.0f ? static_cast<uint32_t>(w + 0.5f) : 0u;
        *height = h > 0.0f ? static_cast<uint32_t>(h + 0.5f) : 0u;
        return WXL_TRUE;
    }
}
