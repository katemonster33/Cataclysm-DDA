#include "inventory_ui.h"

#include <cstdint>
#include <optional>

#include "activity_actor_definitions.h"
#include "cata_assert.h"
#include "cata_utility.h"
#include "catacharset.h"
#include "character.h"
#include "colony.h"
#include "cuboid_rectangle.h"
#include "debug.h"
#include "enums.h"
#include "flag.h"
#include <imgui/imgui.h>
#include "inventory.h"
#include "item.h"
#include "item_category.h"
#include "item_pocket.h"
#include "item_search.h"
#include "item_stack.h"
#include "iteminfo_query.h"
#include "line.h"
#include "make_static.h"
#include "map.h"
#include "map_selector.h"
#include "memory_fast.h"
#include "messages.h"
#include "options.h"
#include "output.h"
#include "point.h"
#include "ret_val.h"
#include "sdltiles.h"
#include "localized_comparator.h"
#include "string_formatter.h"
#include "string_input_popup.h"
#include "translations.h"
#include "type_id.h"
#include "uistate.h"
#include "ui_manager.h"
#include "units.h"
#include "units_utility.h"
#include "vehicle.h"
#include "vehicle_selector.h"
#include "vpart_position.h"

#if defined(__ANDROID__)
#include <SDL_keyboard.h>
#endif

#include <algorithm>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include "cata_imgui.h"


inventory_entry *mouse_hovered_entry = nullptr;

inventory_entry *keyboard_focused_entry = nullptr;
const item_location *entry_to_be_focused = nullptr;

static const item_category_id item_category_BIONIC_FUEL_SOURCE( "BIONIC_FUEL_SOURCE" );
static const item_category_id item_category_INTEGRATED( "INTEGRATED" );
static const item_category_id item_category_ITEMS_WORN( "ITEMS_WORN" );
static const item_category_id item_category_WEAPON_HELD( "WEAPON_HELD" );

namespace
{
using startup_timer = std::chrono::time_point<std::chrono::steady_clock, std::chrono::milliseconds>;
startup_timer tp_start;

void debug_print_timer( startup_timer const &tp, std::string const &msg = "inv_ui setup took" )
{
    startup_timer const tp_now =
        std::chrono::time_point_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() );
    add_msg_debug( debugmode::DF_GAME, "%s: %i ms", msg, ( tp_now - tp ).count() );
}

using item_name_t = std::pair<std::string, std::string>;
using name_cache_t = std::unordered_map<item const *, item_name_t>;
name_cache_t item_name_cache;
int item_name_cache_users = 0;

item_name_t &get_cached_name( item const *it )
{
    auto iter = item_name_cache.find( it );
    if( iter == item_name_cache.end() ) {
        return item_name_cache
               .emplace( it, item_name_t{ remove_color_tags( it->tname( 1, false, 0, true, false ) ),
                                          remove_color_tags( it->tname( 1, true, 0, true, false ) ) } )
               .first->second;
    }

    return iter->second;
}

// get topmost visible parent in an unbroken chain
item_location get_topmost_parent( item_location const &topmost, item_location const &loc,
                                  inventory_selector_preset const &preset )
{
    return preset.is_shown( loc ) ? topmost ? topmost : loc : item_location{};
}

using parent_path_t = std::vector<item_location>;
parent_path_t path_to_top( inventory_entry const &e, inventory_selector_preset const &pr )
{
    item_location it = e.any_item();
    parent_path_t path{ it };
    while( it.has_parent() ) {
        it = it.parent_item();
        if( pr.is_shown( it ) ) {
            path.emplace_back( it );
        }
    }
    return path;
}

using pred_t = std::function<bool( inventory_entry const & )>;
void move_if( std::vector<inventory_entry> &src, std::vector<inventory_entry> &dst,
              pred_t const &pred )
{
    for( auto it = src.begin(); it != src.end(); ) {
        if( pred( *it ) ) {
            if( it->is_item() ) {
                dst.emplace_back( std::move( *it ) );
            }
            it = src.erase( it );
        } else {
            ++it;
        }
    }
}

bool always_yes( const inventory_entry & )
{
    return true;
}

bool return_item( const inventory_entry &entry )
{
    return entry.is_item();
}

bool is_container( const item_location &loc )
{
    return loc.where() == item_location::type::container;
}

bool is_worn_id( item_category_id const &id )
{
    return id == item_category_ITEMS_WORN || id == item_category_INTEGRATED ||
           id == item_category_BIONIC_FUEL_SOURCE;
}

item_category const *wielded_worn_category( item_location const &loc, Character const &u )
{
    if( loc == u.get_wielded_item() ) {
        return &item_category_WEAPON_HELD.obj();
    }
    if( u.is_worn( *loc ) || ( loc.has_parent() && is_worn_ablative( loc.parent_item(), loc ) ) ) {
        if( loc->has_flag( flag_BIONIC_FUEL_SOURCE ) ) {
            return &item_category_BIONIC_FUEL_SOURCE.obj();
        }
        if( loc->has_flag( flag_INTEGRATED ) ) {
            return &item_category_INTEGRATED.obj();
        }
        return &item_category_ITEMS_WORN.obj();
    }
    return nullptr;
}

} // namespace

bool is_worn_ablative( item_location const &container, item_location const &child )
{
    // if the item is in an ablative pocket then put it with the item it is in
    // first do a short circuit test if the parent has ablative pockets at all
    return container->is_ablative() && container->is_worn_by_player() &&
           child.parent_pocket()->get_pocket_data()->ablative;
}

bool inventory_selector::skip_unselectable = false;

struct navigation_mode_data {
    navigation_mode next_mode;
    translation name;
    nc_color color;
};

struct container_data {
    units::volume actual_capacity;
    units::volume total_capacity;
    units::mass actual_capacity_weight;
    units::mass total_capacity_weight;
    units::length max_containable_length;

    std::string to_formatted_string( const bool compact = true ) const {
        std::string string_to_format;
        if( compact ) {
            string_to_format = _( "%s/%s : %s/%s : max %s" );
        } else {
            string_to_format = _( "(remains %s, %s) max length %s" );
        }
        return string_format( string_to_format,
                              unit_to_string( total_capacity - actual_capacity, true, true ),
                              unit_to_string( total_capacity_weight - actual_capacity_weight, true, true ),
                              unit_to_string( max_containable_length, true ) );
    }
};

bool inventory_entry::operator==( const inventory_entry &other ) const
{
    return get_category_ptr() == other.get_category_ptr() && locations == other.locations;
}

class selection_column_preset : public inventory_selector_preset
{
    public:
        selection_column_preset() = default;
        std::string get_caption( const inventory_entry &entry ) const override {
            std::string res;
            const size_t available_count = entry.get_available_count();
            const item_location &item = entry.any_item();

            if( entry.chosen_count > 0 && entry.chosen_count < available_count ) {
                //~ %1$d: chosen count, %2$d: available count
                res += string_format( pgettext( "count", "%1$d of %2$d" ), entry.chosen_count,
                                      available_count ) + " ";
            } else if( available_count != 1 ) {
                res += string_format( "%d ", available_count );
            }
            if( item->is_money() ) {
                cata_assert( available_count == entry.get_stack_size() );
                if( entry.chosen_count > 0 && entry.chosen_count < available_count ) {
                    res += item->display_money( available_count, item->ammo_remaining(),
                                                entry.get_selected_charges() );
                } else {
                    res += item->display_money( available_count, item->ammo_remaining() );
                }
            } else {
                res += item->display_name( available_count );
            }
            return res;
        }

        nc_color get_color( const inventory_entry &entry ) const override {
            Character &player_character = get_player_character();
            if( entry.is_item() ) {
                if( entry.any_item() == player_character.get_wielded_item() ) {
                    return c_light_blue;
                } else if( player_character.is_worn( *entry.any_item() ) ) {
                    return c_cyan;
                }
            }
            return inventory_selector_preset::get_color( entry );
        }
};

void inventory_selector_save_state::serialize( JsonOut &json ) const
{
    json.start_object();
    json.member( "uimode", uimode );
    json.end_object();
}
void inventory_selector_save_state::deserialize( JsonObject const &jo )
{
    jo.read( "uimode", uimode );
}

namespace io
{
template <>
std::string enum_to_string<inventory_selector::uimode>( inventory_selector::uimode mode )
{
    switch( mode ) {
        case inventory_selector::uimode::hierarchy:
            return "hierarchy";
        case inventory_selector::uimode::last:
        case inventory_selector::uimode::categories:
            break;
    }
    return "categories";
}
} // namespace io

static inventory_selector_save_state inventory_sel_default_state{ inventory_selector::uimode::categories };
inventory_selector_save_state inventory_ui_default_state{ inventory_selector::uimode::categories };
inventory_selector_save_state pickup_sel_default_state{ inventory_selector::uimode::categories };
inventory_selector_save_state pickup_ui_default_state{ inventory_selector::uimode::hierarchy };

void save_inv_state( JsonOut &json )
{
    json.member( "inventory_sel_default_state", inventory_sel_default_state );
    json.member( "inventory_ui_state", inventory_ui_default_state );
    json.member( "pickup_sel_state", pickup_sel_default_state );
    json.member( "pickup_ui_state", pickup_ui_default_state );
}

void load_inv_state( const JsonObject &jo )
{
    jo.read( "inventory_sel_default_state", inventory_sel_default_state );
    jo.read( "inventory_ui_state", inventory_ui_default_state );
    jo.read( "pickup_sel_state", pickup_sel_default_state );
    jo.read( "pickup_ui_state", pickup_ui_default_state );
}

void uistatedata::serialize( JsonOut &json ) const
{
    const unsigned int input_history_save_max = 25;
    json.start_object();

    transfer_save.serialize( json, "transfer_save_" );
    save_inv_state( json );

    /**** if you want to save whatever so it's whatever when the game is started next, declare here and.... ****/
    // non array stuffs
    json.member( "ags_pay_gas_selected_pump", ags_pay_gas_selected_pump );
    json.member( "adv_inv_container_location", adv_inv_container_location );
    json.member( "adv_inv_container_index", adv_inv_container_index );
    json.member( "adv_inv_container_in_vehicle", adv_inv_container_in_vehicle );
    json.member( "adv_inv_container_type", adv_inv_container_type );
    json.member( "adv_inv_container_content_type", adv_inv_container_content_type );
    json.member( "editmap_nsa_viewmode", editmap_nsa_viewmode );
    json.member( "overmap_blinking", overmap_blinking );
    json.member( "overmap_show_overlays", overmap_show_overlays );
    json.member( "overmap_show_map_notes", overmap_show_map_notes );
    json.member( "overmap_show_land_use_codes", overmap_show_land_use_codes );
    json.member( "overmap_show_city_labels", overmap_show_city_labels );
    json.member( "overmap_show_hordes", overmap_show_hordes );
    json.member( "overmap_show_forest_trails", overmap_show_forest_trails );
    json.member( "vmenu_show_items", vmenu_show_items );
    json.member( "list_item_sort", list_item_sort );
    json.member( "list_item_filter_active", list_item_filter_active );
    json.member( "list_item_downvote_active", list_item_downvote_active );
    json.member( "list_item_priority_active", list_item_priority_active );
    json.member( "construction_filter", construction_filter );
    json.member( "last_construction", last_construction );
    json.member( "construction_tab", construction_tab );
    json.member( "hidden_recipes", hidden_recipes );
    json.member( "favorite_recipes", favorite_recipes );
    json.member( "expanded_recipes", expanded_recipes );
    json.member( "read_recipes", read_recipes );
    json.member( "recent_recipes", recent_recipes );
    json.member( "bionic_ui_sort_mode", bionic_sort_mode );
    json.member( "overmap_debug_weather", overmap_debug_weather );
    json.member( "overmap_visible_weather", overmap_visible_weather );
    json.member( "overmap_debug_mongroup", overmap_debug_mongroup );
    json.member( "distraction_noise", distraction_noise );
    json.member( "distraction_pain", distraction_pain );
    json.member( "distraction_attack", distraction_attack );
    json.member( "distraction_hostile_close", distraction_hostile_close );
    json.member( "distraction_hostile_spotted", distraction_hostile_spotted );
    json.member( "distraction_conversation", distraction_conversation );
    json.member( "distraction_asthma", distraction_asthma );
    json.member( "distraction_dangerous_field", distraction_dangerous_field );
    json.member( "distraction_weather_change", distraction_weather_change );
    json.member( "distraction_hunger", distraction_hunger );
    json.member( "distraction_thirst", distraction_thirst );
    json.member( "distraction_temperature", distraction_temperature );
    json.member( "distraction_mutation", distraction_mutation );
    json.member( "numpad_navigation", numpad_navigation );

    json.member( "input_history" );
    json.start_object();
    for( const auto &e : input_history ) {
        json.member( e.first );
        const std::vector<std::string> &history = e.second;
        json.start_array();
        int save_start = 0;
        if( history.size() > input_history_save_max ) {
            save_start = history.size() - input_history_save_max;
        }
        for( std::vector<std::string>::const_iterator hit = history.begin() + save_start;
             hit != history.end(); ++hit ) {
            json.write( *hit );
        }
        json.end_array();
    }
    json.end_object(); // input_history

    json.member( "lastreload", lastreload );

    json.end_object();
}

void uistatedata::deserialize( const JsonObject &jo )
{
    jo.allow_omitted_members();

    transfer_save.deserialize( jo, "transfer_save_" );
    load_inv_state( jo );
    // the rest
    jo.read( "ags_pay_gas_selected_pump", ags_pay_gas_selected_pump );
    jo.read( "adv_inv_container_location", adv_inv_container_location );
    jo.read( "adv_inv_container_index", adv_inv_container_index );
    jo.read( "adv_inv_container_in_vehicle", adv_inv_container_in_vehicle );
    jo.read( "adv_inv_container_type", adv_inv_container_type );
    jo.read( "adv_inv_container_content_type", adv_inv_container_content_type );
    jo.read( "editmap_nsa_viewmode", editmap_nsa_viewmode );
    jo.read( "overmap_blinking", overmap_blinking );
    jo.read( "overmap_show_overlays", overmap_show_overlays );
    jo.read( "overmap_show_map_notes", overmap_show_map_notes );
    jo.read( "overmap_show_land_use_codes", overmap_show_land_use_codes );
    jo.read( "overmap_show_city_labels", overmap_show_city_labels );
    jo.read( "overmap_show_hordes", overmap_show_hordes );
    jo.read( "overmap_show_forest_trails", overmap_show_forest_trails );
    jo.read( "hidden_recipes", hidden_recipes );
    jo.read( "favorite_recipes", favorite_recipes );
    jo.read( "expanded_recipes", expanded_recipes );
    jo.read( "read_recipes", read_recipes );
    jo.read( "recent_recipes", recent_recipes );
    jo.read( "bionic_ui_sort_mode", bionic_sort_mode );
    jo.read( "overmap_debug_weather", overmap_debug_weather );
    jo.read( "overmap_visible_weather", overmap_visible_weather );
    jo.read( "overmap_debug_mongroup", overmap_debug_mongroup );
    jo.read( "distraction_noise", distraction_noise );
    jo.read( "distraction_pain", distraction_pain );
    jo.read( "distraction_attack", distraction_attack );
    jo.read( "distraction_hostile_close", distraction_hostile_close );
    jo.read( "distraction_hostile_spotted", distraction_hostile_spotted );
    jo.read( "distraction_conversation", distraction_conversation );
    jo.read( "distraction_asthma", distraction_asthma );
    jo.read( "distraction_dangerous_field", distraction_dangerous_field );
    jo.read( "distraction_weather_change", distraction_weather_change );
    jo.read( "distraction_hunger", distraction_hunger );
    jo.read( "distraction_thirst", distraction_thirst );
    jo.read( "distraction_temperature", distraction_temperature );
    jo.read( "distraction_mutation", distraction_mutation );
    jo.read( "numpad_navigation", numpad_navigation );

    if( !jo.read( "vmenu_show_items", vmenu_show_items ) ) {
        // This is an old save: 1 means view items, 2 means view monsters,
        // -1 means uninitialized
        vmenu_show_items = jo.get_int( "list_item_mon", -1 ) != 2;
    }

    jo.read( "list_item_sort", list_item_sort );
    jo.read( "list_item_filter_active", list_item_filter_active );
    jo.read( "list_item_downvote_active", list_item_downvote_active );
    jo.read( "list_item_priority_active", list_item_priority_active );

    jo.read( "construction_filter", construction_filter );
    jo.read( "last_construction", last_construction );
    jo.read( "construction_tab", construction_tab );

    for( const JsonMember member : jo.get_object( "input_history" ) ) {
        std::vector<std::string> &v = gethistory( member.name() );
        v.clear();
        for( const std::string line : member.get_array() ) {
            v.push_back( line );
        }
    }
    // fetch list_item settings from input_history
    if( !gethistory( "item_filter" ).empty() ) {
        list_item_filter = gethistory( "item_filter" ).back();
    }
    if( !gethistory( "list_item_downvote" ).empty() ) {
        list_item_downvote = gethistory( "list_item_downvote" ).back();
    }
    if( !gethistory( "list_item_priority" ).empty() ) {
        list_item_priority = gethistory( "list_item_priority" ).back();
    }

    jo.read( "lastreload", lastreload );
}

static const selection_column_preset selection_preset{};

bool inventory_entry::is_hidden( std::optional<bool> const &hide_entries_override ) const
{
    if( !is_item() ) {
        return false;
    }

    if( is_collation_entry() && collation_meta->collapsed ) {
        return true;
    }

    if( !topmost_parent ) {
        return false;
    }

    item_location item = locations.front();
    if( hide_entries_override && topmost_parent && topmost_parent->is_container() ) {
        return *hide_entries_override;
    }
    while( item.has_parent() && item != topmost_parent ) {
        if( item.parent_pocket()->settings.is_collapsed() ) {
            return true;
        }
        item = item.parent_item();
    }
    return false;
}

int inventory_entry::get_total_charges() const
{
    int result = 0;
    for( const item_location &location : locations ) {
        result += location->charges;
    }
    return result;
}

int inventory_entry::get_selected_charges() const
{
    cata_assert( chosen_count <= locations.size() );
    int result = 0;
    for( size_t i = 0; i < chosen_count; ++i ) {
        const item_location &location = locations[i];
        result += location->charges;
    }
    return result;
}

size_t inventory_entry::get_available_count() const
{
    if( locations.size() == 1 ) {
        return any_item()->count();
    } else {
        return locations.size();
    }
}

int inventory_entry::get_invlet() const
{
    if( custom_invlet != INT_MIN ) {
        return custom_invlet;
    }
    if( !is_item() ) {
        return '\0';
    }
    return any_item()->invlet;
}

nc_color inventory_entry::get_invlet_color() const
{
    if( !is_selectable() ) {
        return c_dark_gray;
    } else if( get_player_character().inv->assigned_invlet.count( get_invlet() ) ) {
        return c_yellow;
    } else {
        return c_white;
    }
}

void inventory_entry::update_cache()
{
    item_name_t &names = get_cached_name( &*any_item() );
    cached_name = &names.first;
    cached_name_full = &names.second;
}

void inventory_entry::cache_denial( inventory_selector_preset const &preset ) const
{
    if( !denial ) {
        denial.emplace( preset.get_denial( *this ) );
        enabled = denial->empty();
    }
    if( is_collation_header() ) {
        collation_meta->enabled = denial->empty();
    }
}

const item_category *inventory_entry::get_category_ptr() const
{
    if( custom_category != nullptr ) {
        return custom_category;
    }
    if( !is_item() ) {
        return nullptr;
    }
    return &any_item()->get_category_of_contents();
}

bool inventory_column::activatable() const
{
    return std::any_of( entries.begin(), entries.end(), [this]( const inventory_entry & e ) {
        return e.is_highlightable( skip_unselectable );
    } );
}

inventory_entry *inventory_column::find_by_invlet( int invlet ) const
{
    for( const inventory_entry &elem : entries ) {
        if( elem.is_item() && elem.get_invlet() == invlet ) {
            return const_cast<inventory_entry *>( &elem );
        }
    }
    return nullptr;
}

inventory_entry *inventory_column::find_by_location( item_location const &loc, bool hidden ) const
{
    std::vector<inventory_entry> const &ents = hidden ? entries_hidden : entries;
    for( const inventory_entry &elem : ents ) {
        if( elem.is_item() ) {
            for( const item_location &it : elem.locations ) {
                if( it == loc ) {
                    return const_cast<inventory_entry *>( &elem );
                }
            }
        }
    }
    return nullptr;
}

void inventory_column::toggle_skip_unselectable( const bool skip )
{
    skip_unselectable = skip;
}

inventory_selector_preset::inventory_selector_preset()
{
    append_cell(
    std::function<std::string( const inventory_entry & )>( [ this ]( const inventory_entry & entry ) {
        return get_caption( entry );
    } ) );
}

bool inventory_selector_preset::sort_compare( const inventory_entry &lhs,
        const inventory_entry &rhs ) const
{
    auto const sort_key = []( inventory_entry const & e ) {
        return std::make_tuple( *e.cached_name, *e.cached_name_full, e.generation );
    };
    return localized_compare( sort_key( lhs ), sort_key( rhs ) );
}

bool inventory_selector_preset::cat_sort_compare( const inventory_entry &lhs,
        const inventory_entry &rhs ) const
{
    return *lhs.get_category_ptr() < *rhs.get_category_ptr();
}

nc_color inventory_selector_preset::get_color( const inventory_entry &entry ) const
{
    return entry.is_item() ? entry.any_item()->color_in_inventory() : c_magenta;
}

std::function<bool( const inventory_entry & )> inventory_selector_preset::get_filter(
    const std::string &filter ) const
{
    auto item_filter = basic_item_filter( filter );

    return [item_filter]( const inventory_entry & e ) {
        return item_filter( *e.any_item() );
    };
}

std::string inventory_selector_preset::get_caption( const inventory_entry &entry ) const
{
    size_t count = entry.get_stack_size();
    std::string disp_name;
    if( entry.any_item()->is_money() ) {
        disp_name = entry.any_item()->display_money( count, entry.any_item()->ammo_remaining() );
    } else if( entry.is_collation_header() && entry.any_item()->count_by_charges() ) {
        item temp( *entry.any_item() );
        temp.charges = entry.get_total_charges();
        disp_name = temp.display_name();
        count = 1;
    } else {
        disp_name = entry.any_item()->display_name( count );
    }

    return ( count > 1 ) ? string_format( "%d %s", count, disp_name ) : disp_name;
}

std::string inventory_selector_preset::get_denial( const inventory_entry &entry ) const
{
    return entry.is_item() ? get_denial( entry.any_item() ) : std::string();
}

std::string inventory_selector_preset::get_cell_text( const inventory_entry &entry,
        size_t cell_index ) const
{
    if( cell_index >= cells.size() ) {
        debugmsg( "Invalid cell index %d.", cell_index );
        return "it's a bug!";
    }
    if( !entry ) {
        return std::string();
    } else if( entry.is_item() ) {
        std::string text = cells[cell_index].get_text( entry );
        const item &actual_item = *entry.locations.front();
        const std::string info_display = get_option<std::string>( "DETAILED_CONTAINERS" );
        // if we want no additional info skip this
        if( info_display != "NONE" ) {
            // if we want additional info for all items or it is worn then add the additional info
            if( info_display == "ALL" || ( info_display == "WORN" &&
                                           is_worn_id( entry.get_category_ptr()->get_id() ) &&
                                           actual_item.is_worn_by_player() ) ) {
                if( cell_index == 0 && !text.empty() &&
                    actual_item.is_container() && actual_item.has_unrestricted_pockets() ) {
                    const units::volume total_capacity = actual_item.get_total_capacity( true );
                    const units::mass total_capacity_weight = actual_item.get_total_weight_capacity( true );
                    const units::length max_containable_length = actual_item.max_containable_length( true );

                    const units::volume actual_capacity = actual_item.get_total_contained_volume( true );
                    const units::mass actual_capacity_weight = actual_item.get_total_contained_weight( true );

                    container_data container_data = {
                        actual_capacity,
                        total_capacity,
                        actual_capacity_weight,
                        total_capacity_weight,
                        max_containable_length
                    };
                    std::string formatted_string = container_data.to_formatted_string( false );

                    text = text + string_format( " %s", formatted_string );
                }
            }
        }
        return text;
    } else if( cell_index != 0 ) {
        return replace_colors( cells[cell_index].title );
    } else {
        return entry.get_category_ptr()->name();
    }
}

bool inventory_selector_preset::is_stub_cell( const inventory_entry &entry,
        size_t cell_index ) const
{
    if( !entry.is_item() ) {
        return false;
    }
    const std::string &text = entry.get_entry_cell_cache( *this ).text[ cell_index ];
    return text.empty() || text == cells[cell_index].stub;
}

void inventory_selector_preset::append_cell( const
        std::function<std::string( const item_location & )> &func,
        const std::string &title, const std::string &stub )
{
    // Don't capture by reference here. The func should be able to die earlier than the object itself
    append_cell( std::function<std::string( const inventory_entry & )>( [ func ](
    const inventory_entry & entry ) {
        return func( entry.any_item() );
    } ), title, stub );
}

void inventory_selector_preset::append_cell( const
        std::function<std::string( const inventory_entry & )> &func,
        const std::string &title, const std::string &stub )
{
    const auto iter = std::find_if( cells.begin(), cells.end(), [ &title ]( const cell_t &cell ) {
        return cell.title == title;
    } );
    if( iter != cells.end() ) {
        debugmsg( "Tried to append a duplicate cell \"%s\": ignored.", title.c_str() );
        return;
    }
    cells.emplace_back( func, title, stub );
}

std::string inventory_selector_preset::cell_t::get_text( const inventory_entry &entry ) const
{
    return replace_colors( func( entry ) );
}

const item_location &inventory_holster_preset::get_holster() const
{
    return holster;
}

bool inventory_holster_preset::is_shown( const item_location &contained ) const
{
    if( contained == holster ) {
        return false;
    }
    if( contained.eventually_contains( holster ) || holster.eventually_contains( contained ) ) {
        return false;
    }
    if( !is_container( contained ) && contained->made_of( phase_id::LIQUID ) &&
        !contained->is_frozen_liquid() ) {
        // spilt liquid cannot be picked up
        return false;
    }
    if( contained->made_of( phase_id::LIQUID ) && !holster->is_watertight_container() ) {
        return false;
    }
    item item_copy( *contained );
    item_copy.charges = 1;
    item_location parent = contained.has_parent() ? contained.parent_item() : item_location();
    if( !holster->can_contain( item_copy, false, false, true, parent ).success() ) {
        return false;
    }

    //only hide if it is in the toplevel of holster (to allow shuffling of items inside a bag)
    for( const item *it : holster->all_items_top() ) {
        if( it == contained.get_item() ) {
            return false;
        }
    }

    if( contained->is_bucket_nonempty() ) {
        return false;
    }
    if( !holster->all_pockets_rigid() &&
        !holster.parents_can_contain_recursive( &item_copy ) ) {
        return false;
    }
    return true;
}

std::string inventory_holster_preset::get_denial( const item_location &it ) const
{
    if( who->is_worn( *it ) ) {
        ret_val<void> const ret = who->can_takeoff( *it );
        if( !ret.success() ) {
            return ret.str();
        }
    }
    return {};
}

void inventory_column::highlight( size_t new_index, scroll_direction dir )
{
    if( new_index < entries.size() ) {
        if( !entries[new_index].is_highlightable( skip_unselectable ) ) {
            new_index = next_highlightable_index( new_index, dir );
        }
        if( !parent_selector->multiselect ) {
            for( inventory_entry &entry : entries ) {
                entry.is_selected = false;
            }
        }
        entries[new_index].is_selected = true;
    }
}

void inventory_column::calculate_cell_width( size_t index )
{
    cells[index].current_width = 0;
    for( auto entry : entries ) {
        inventory_entry::entry_cell_cache_t cache = entry.get_entry_cell_cache( parent_selector->preset );
        std::string text_stripped = remove_color_tags( cache.text[index] );
        if( text_stripped.length() > cells[index].current_width ) {
            cells[index].current_width = text_stripped.length();
        }
    }
    cells[index].current_width = ( cells[index].current_width + 1 ) * fontwidth;
}

size_t inventory_column::next_highlightable_index( size_t index, scroll_direction dir ) const
{
    if( entries.empty() ) {
        return index;
    }
    // limit index to the space of the size of entries
    index = index % entries.size();
    size_t new_index = index;
    do {
        // 'new_index' incremented by 'dir' using division remainder (number of entries) to loop over the entries.
        // Negative step '-k' (backwards) is equivalent to '-k + N' (forward), where:
        //     N = entries.size()  - number of elements,
        //     k = |step|          - absolute step (k <= N).
        new_index = ( new_index + static_cast<int>( dir ) + entries.size() ) % entries.size();
    } while( new_index != index &&
             !entries[new_index].is_highlightable( skip_unselectable ) );

    if( !entries[new_index].is_highlightable( skip_unselectable ) ) {
        return static_cast<size_t>( -1 );
    }

    return new_index;
}

size_t inventory_column::get_entry_cell_width( const inventory_entry &entry,
        size_t cell_index ) const
{
    size_t res = utf8_width( entry.get_entry_cell_cache( preset ).text[cell_index], true );

    if( cell_index == 0 ) {
        res += get_entry_indent( entry );
    }

    return res;
}

size_t inventory_column::get_cells_width() const
{
    return std::accumulate( cells.begin(), cells.end(), static_cast<size_t>( 0 ), []( size_t lhs,
    const cell_t &cell ) {
        return lhs + cell.current_width;
    } );
}

void inventory_column::set_filter( const std::string &/* filter */ )
{
    paging_is_valid = false;
}

void selection_column::set_filter( const std::string & )
{
    // always show all selected items
    inventory_column::set_filter( std::string() );
}

void inventory_entry::make_entry_cell_cache(
    inventory_selector_preset const &preset, bool update_only ) const
{
    if( update_only && !entry_cell_cache ) {
        return;
    }
    entry_cell_cache.emplace( entry_cell_cache_t{
        preset.get_color( *this ),
        { preset.get_cells_count(), std::string() } } );

    for( size_t i = 0, n = preset.get_cells_count(); i < n; ++i ) {
        entry_cell_cache->text[i] = preset.get_cell_text( *this, i );
    }
}

void inventory_entry::reset_entry_cell_cache() const
{
    entry_cell_cache.reset();
}

const inventory_entry::entry_cell_cache_t &inventory_entry::get_entry_cell_cache(
    inventory_selector_preset const &preset ) const
{
    //lang check here is needed to rebuild cache when using "Toggle language to English" option
    if( !entry_cell_cache ||
        entry_cell_cache->lang_version != detail::get_current_language_version() ) {
        make_entry_cell_cache( preset, false );
        cache_denial( preset );
        cata_assert( entry_cell_cache.has_value() );
        entry_cell_cache->lang_version = detail::get_current_language_version();
    }

    return *entry_cell_cache;
}

inventory_column::inventory_column( inventory_selector *parent,
                                    const inventory_selector_preset &preset ) :
    preset( preset )
{
    cells.resize( preset.get_cells_count() );
    parent_selector = parent;
}

bool inventory_column::has_available_choices() const
{
    if( !allows_selecting() || !activatable() ) {
        return false;
    }
    return std::any_of( entries.begin(), entries.end(), []( inventory_entry const & e ) {
        return e.is_item() && e.enabled;
    } );
}

bool inventory_column::is_selected_by_category( const inventory_entry &entry ) const
{
    return entry.is_selectable() && mode == navigation_mode::CATEGORY
           && entry.get_category_ptr() == get_highlighted().get_category_ptr();
}

const inventory_entry &inventory_column::get_highlighted() const
{
    for( const inventory_entry &entry : entries ) {
        if( entry && !entry.is_category() && entry.is_selected ) {
            return entry;
        }
    }
    // clang complains if we use the default constructor here
    static const inventory_entry dummy( nullptr );
    return dummy;
}

size_t inventory_column::get_highlighted_index() const
{
    for( size_t index = 0; index < entries.size(); index++ ) {
        if( entries[index].is_selected ) {
            return index;
        }
    }
    return SIZE_MAX;
}

inventory_entry &inventory_column::get_highlighted()
{
    return const_cast<inventory_entry &>( const_cast<const inventory_column *>
                                          ( this )->get_highlighted() );
}

std::vector<inventory_entry *> inventory_column::get_all_selected() const
{
    const auto filter_to_selected = [&]( const inventory_entry & entry ) {
        return entry.is_selected && entry.is_selectable();
    };
    return get_entries( filter_to_selected );
}

void inventory_column::_get_entries( get_entries_t *res, entries_t const &ent,
                                     const ffilter_t &filter_func ) const
{
    if( allows_selecting() ) {
        for( const inventory_entry &elem : ent ) {
            if( filter_func( elem ) ) {
                res->push_back( const_cast<inventory_entry *>( &elem ) );
            }
        }
    }
}

void inventory_selector::refresh_active_column()
{
    if( active_column_index != SIZE_MAX && columns[active_column_index]->activatable() ) {
        toggle_active_column( scroll_direction::FORWARD );
    }
}

inventory_column::get_entries_t inventory_column::get_entries( const ffilter_t &filter_func,
        bool include_hidden ) const
{
    get_entries_t res;

    if( allows_selecting() ) {
        _get_entries( &res, entries, filter_func );
        if( include_hidden ) {
            _get_entries( &res, entries_hidden, filter_func );
        }
    }

    return res;
}

void inventory_column::set_stack_favorite( inventory_entry &entry,
        const bool favorite )
{
    for( item_location &loc : entry.locations ) {
        loc->set_favorite( favorite );
    }
    entry.make_entry_cell_cache( preset );
}

void inventory_column::set_collapsed( inventory_entry &entry, const bool collapse )
{
    bool collapsed = false;
    if( entry.is_collation_header() ) {
        entry.collation_meta->collapsed = collapse;
        collapsed = true;
    } else {
        std::vector<item_location> &locations = entry.locations;

        for( item_location &loc : locations ) {
            for( item_pocket *pocket : loc->get_all_standard_pockets() ) {
                pocket->settings.set_collapse( collapse );
                collapsed = true;
            }
        }
    }

    if( collapsed ) {
        entry.collapsed = collapse;
        paging_is_valid = false;
        entry.make_entry_cell_cache( preset );
    }
}

void inventory_column::on_input( const inventory_input &input )
{
    if( visible() ) {
        //if( input.action == "DOWN" ) {
        //    move_selection( scroll_direction::FORWARD );
        //} else if( input.action == "UP" ) {
        //    move_selection( scroll_direction::BACKWARD );
        //} else if( input.action == "PAGE_DOWN" ) {
        //    move_selection_page( scroll_direction::FORWARD );
        //} else if( input.action == "PAGE_UP" ) {
        //    move_selection_page( scroll_direction::BACKWARD );
        //} else if( input.action == "SCROLL_DOWN" ) {
        //    scroll_selection_page( scroll_direction::FORWARD );
        //} else if( input.action == "SCROLL_UP" ) {
        //    scroll_selection_page( scroll_direction::BACKWARD );
        //} else if( input.action == "HOME" ) {
        //    highlight( 0, scroll_direction::FORWARD );
        //} else if( input.action == "END" ) {
        //    highlight( entries.size() - 1, scroll_direction::BACKWARD );
        if( input.action == "TOGGLE_FAVORITE" ) {
            inventory_entry &selected = get_highlighted();
            if( selected ) {
                set_stack_favorite( selected, !selected.any_item()->is_favorite );
            }
        } else if( input.action == "SHOW_HIDE_CONTENTS" ) {
            inventory_entry &selected = get_highlighted();
            if( selected ) {
                selected.collapsed ? set_collapsed( selected, false ) : set_collapsed( selected, true );
            }
        }
    }

    if( input.action == "TOGGLE_FAVORITE" ) {
        // Favoriting items in one column may change item names in another column
        // if that column contains an item that contains the favorited item. So
        // we invalidate every column on TOGGLE_FAVORITE action.
        paging_is_valid = false;
    }
}

void inventory_column::on_change( const inventory_entry &/* entry */ )
{
    // stub
}

inventory_entry *inventory_column::add_entry( const inventory_entry &entry )
{
    entries_t &dest = entry.is_hidden( hide_entries_override ) ? entries_hidden : entries;
    if( auto it = std::find( dest.begin(), dest.end(), entry ); it != dest.end() ) {
        debugmsg( "Tried to add a duplicate entry." );
        return &*it;
    }
    paging_is_valid = false;
    if( entry.is_item() ) {
        item_location entry_item = entry.locations.front();

        auto entry_with_loc = std::find_if( dest.begin(),
        dest.end(), [&entry, &entry_item, this]( const inventory_entry & e ) {
            if( !e.is_item() ) {
                return false;
            }
            item_location found_entry_item = e.locations.front();
            return !e.is_collated() &&
                   e.get_category_ptr() == entry.get_category_ptr() &&
                   entry_item.where() == found_entry_item.where() &&
                   entry_item.position() == found_entry_item.position() &&
                   entry_item.parent_item() == found_entry_item.parent_item() &&
                   entry_item->is_collapsed() == found_entry_item->is_collapsed() &&
                   entry_item->display_stacked_with( *found_entry_item, preset.get_checking_components() );
        } );
        if( entry_with_loc != dest.end() ) {
            std::vector<item_location> &locations = entry_with_loc->locations;
            std::move( entry.locations.begin(), entry.locations.end(), std::back_inserter( locations ) );
            return &*entry_with_loc;
        }
    }

    dest.emplace_back( entry );
    inventory_entry &newent = dest.back();
    newent.update_cache();

    return &newent;
}

void inventory_column::move_entries_to( inventory_column &dest )
{
    std::move( entries.begin(), entries.end(), std::back_inserter( dest.entries ) );
    std::move( entries_hidden.begin(), entries_hidden.end(),
               std::back_inserter( dest.entries_hidden ) );
    dest.paging_is_valid = false;
    clear();
}

bool inventory_column::sort_compare( inventory_entry const &lhs, inventory_entry const &rhs )
{
    if( lhs.is_selectable() != rhs.is_selectable() ) {
        return lhs.is_selectable(); // Disabled items always go last
    }
    Character &player_character = get_player_character();
    // Place favorite items and items with an assigned inventory letter first,
    // since the player cared enough to assign them
    const bool left_has_invlet =
        player_character.inv->assigned_invlet.count( lhs.any_item()->invlet ) != 0;
    const bool right_has_invlet =
        player_character.inv->assigned_invlet.count( rhs.any_item()->invlet ) != 0;
    if( left_has_invlet != right_has_invlet ) {
        return left_has_invlet;
    }
    const bool left_fav = lhs.any_item()->is_favorite;
    const bool right_fav = rhs.any_item()->is_favorite;
    if( left_fav != right_fav ) {
        return left_fav;
    }

    return preset.sort_compare( lhs, rhs );
}

bool inventory_column::indented_sort_compare( inventory_entry const &lhs,
        inventory_entry const &rhs )
{
    // place children below all parents
    parent_path_t const path_lhs = path_to_top( lhs, preset );
    parent_path_t const path_rhs = path_to_top( rhs, preset );
    parent_path_t::size_type const common_depth = std::min( path_lhs.size(), path_rhs.size() );
    parent_path_t::size_type li = path_lhs.size() - common_depth;
    parent_path_t::size_type ri = path_rhs.size() - common_depth;
    item_location p_lhs = path_lhs[li];
    item_location p_rhs = path_rhs[ri];
    if( p_lhs == p_rhs ) {
        return path_lhs.size() < path_rhs.size();
    }
    // otherwise sort the entries below their lowest common ancestor
    while( li < path_lhs.size() && path_lhs[li] != path_rhs[ri] ) {
        p_lhs = path_lhs[li++];
        p_rhs = path_rhs[ri++];
    }

    inventory_entry ep_lhs( { p_lhs }, nullptr, true, 0, lhs.generation );
    inventory_entry ep_rhs( { p_rhs }, nullptr, true, 0, rhs.generation );
    ep_lhs.update_cache();
    ep_rhs.update_cache();
    return sort_compare( ep_lhs, ep_rhs );
}

bool inventory_column::collated_sort_compare( inventory_entry const &lhs,
        inventory_entry const &rhs )
{
    if( lhs.is_collated() && lhs.collation_meta == rhs.collation_meta ) {
        if( lhs.is_collation_header() ) {
            return true;
        }
        if( rhs.is_collation_header() ) {
            return false;
        }
        return sort_compare( lhs, rhs );
    }

    item_location p_lhs = lhs.is_collated() ? lhs.collation_meta->tip : lhs.any_item();
    item_location p_rhs = rhs.is_collated() ? rhs.collation_meta->tip : rhs.any_item();

    inventory_entry ep_lhs(
    { p_lhs }, nullptr, lhs.is_collated() ? lhs.collation_meta->enabled : lhs.is_selectable(),
    0, lhs.generation );
    inventory_entry ep_rhs(
    { p_rhs }, nullptr, rhs.is_collated() ? rhs.collation_meta->enabled : rhs.is_selectable(),
    0, rhs.generation );
    ep_lhs.update_cache();
    ep_rhs.update_cache();
    if( indent_entries() ) {
        return indented_sort_compare( ep_lhs, ep_rhs );
    }
    return sort_compare( ep_lhs, ep_rhs );
}

void inventory_column::collate()
{
    for( auto outer = entries.begin(); outer != entries.end(); ++outer ) {
        if( !outer->is_item() || outer->is_collated() || outer->chevron ) {
            continue;
        }
        for( auto e = std::next( outer ); e != entries.end(); ) {
            if( e->is_item() && e->get_category_ptr() == outer->get_category_ptr() &&
                e->any_item()->is_favorite == outer->any_item()->is_favorite &&
                e->any_item()->typeId() == outer->any_item()->typeId() &&
                ( !indent_entries() ||
                  e->any_item().parent_item() == outer->any_item().parent_item() ) &&
                ( e->is_collation_header() || !e->chevron ) &&
                e->any_item()->is_same_relic( *outer->any_item() ) ) {

                if( !outer->is_collated() ) {
                    outer->collation_meta = std::make_shared<collation_meta_t>(
                                                collation_meta_t{ outer->any_item(), true, outer->is_selectable() } );

                    entries_hidden.emplace_back( *outer );

                    outer->chevron = true;
                    set_collapsed( *outer, true );
                    outer->reset_entry_cell_cache(); // needed when switching UI modes
                }
                e->collation_meta = outer->collation_meta;
                std::copy( e->locations.begin(), e->locations.end(),
                           std::back_inserter( outer->locations ) );
                entries_hidden.emplace_back( std::move( *e ) );
                e = entries.erase( e );
            } else {
                ++e;
            }
        }
    }
    _collated = true;
}

void inventory_column::_reset_collation( entries_t &entries )
{
    for( auto iter = entries.begin(); iter != entries.end(); ) {
        if( iter->is_collation_header() ) {
            iter = entries.erase( iter );
        } else {
            iter->reset_collation();
            ++iter;
        }
    }
}

void inventory_column::uncollate()
{
    if( _collated ) {
        _reset_collation( entries );
        _reset_collation( entries_hidden );
        _collated = false;
    }
}

void inventory_column::prepare_paging( const std::string &filter )
{
    if( paging_is_valid ) {
        return;
    }

    const auto filter_fn = filter_from_string<inventory_entry>(
    filter, [this]( const std::string & filter ) {
        return preset.get_filter( filter );
    } );

    const auto is_visible = [&filter_fn, &filter, this]( inventory_entry const & it ) {
        return it.is_item() &&
               ( filter_fn( it ) &&
                 ( ( !filter.empty() && !it.is_collation_entry() ) || !it.is_hidden( hide_entries_override ) ) );
    };
    const auto is_not_visible = [&is_visible, this]( inventory_entry const & it ) {
        it.cache_denial( preset ); // do it here since we're looping over all visible entries anyway
        return !is_visible( it );
    };

    // restore entries revealed by SHOW_HIDE_CONTENTS or filter
    move_if( entries_hidden, entries, is_visible );
    // remove entries hidden by SHOW_HIDE_CONTENTS
    move_if( entries, entries_hidden, is_not_visible );

    // Then sort them with respect to categories
    std::stable_sort( entries.begin(), entries.end(),
    [this]( const inventory_entry & lhs, const inventory_entry & rhs ) {
        if( *lhs.get_category_ptr() == *rhs.get_category_ptr() ) {
            if( _collated ) {
                return collated_sort_compare( lhs, rhs );
            }
            if( indent_entries() ) {
                return indented_sort_compare( lhs, rhs );
            }

            return sort_compare( lhs, rhs );
        }
        return preset.cat_sort_compare( lhs, rhs );
    } );

    if( !_collated && collate_entries() ) {
        collate();
    }

    // Recover categories
    const item_category *current_category = nullptr;
    for( auto iter = entries.begin(); iter != entries.end(); ++iter ) {
        if( iter->get_category_ptr() == current_category ) {
            continue;
        }
        current_category = iter->get_category_ptr();
        iter = entries.insert( iter, inventory_entry( current_category ) );
    }
    paging_is_valid = true;
    // Select the uppermost possible entry
    //const size_t ind = highlighted_index >= entries.size() ? 0 : highlighted_index;
    //highlight( ind, ind ? scroll_direction::BACKWARD : scroll_direction::FORWARD );
}

void inventory_column::clear()
{
    entries.clear();
    entries_hidden.clear();
    paging_is_valid = false;
}

bool inventory_column::highlight( const item_location &loc, bool front_only )
{
    for( inventory_entry &ent : entries ) {
        if( ent.is_item() &&
            ( ( !front_only && std::find( ent.locations.begin(), ent.locations.end(), loc ) !=
                ent.locations.end() ) ||
              ( !ent.is_collation_header() && ent.locations.front() == loc ) ) ) {
            entry_to_be_focused = &ent.any_item();
            return true;
        }
    }
    return false;
}

void inventory_column::deselect_all_except( const inventory_entry &item )
{
    for( inventory_entry &ent : entries ) {
        if( ent != item ) {
            ent.is_selected = false;
        }
    }
}

size_t inventory_column::get_entry_indent( const inventory_entry &entry ) const
{
    if( !entry.is_item() ) {
        return 0;
    }

    size_t res = 2;
    if( get_option<bool>( "ITEM_SYMBOLS" ) ) {
        res += 2;
    }
    if( allows_selecting() && activatable() && parent_selector->multiselect ) {
        res += 2;
    }
    if( entry.is_item() ) {
        if( collate_entries() && entry.is_collation_entry() ) {
            res += 2;
        }
        if( indent_entries() ) {
            res += entry.indent;
        }
    }

    return res;
}

int inventory_column::reassign_custom_invlets( const Character &p, int min_invlet, int max_invlet )
{
    int cur_invlet = min_invlet;
    for( inventory_entry &elem : entries ) {
        // Only items on map/in vehicles: those that the player does not possess.
        if( elem.is_selectable() && !p.has_item( *elem.any_item() ) ) {
            elem.custom_invlet = cur_invlet <= max_invlet ? cur_invlet++ : '\0';
        }
    }
    return cur_invlet;
}

int inventory_column::reassign_custom_invlets( int cur_idx, const std::string_view pickup_chars )
{
    for( inventory_entry &elem : entries ) {
        // Only items on map/in vehicles: those that the player does not possess.
        if( elem.is_selectable() && elem.any_item()->invlet <= '\0' ) {
            elem.custom_invlet =
                static_cast<uint8_t>(
                    cur_idx < static_cast<int>( pickup_chars.size() ) ?
                    pickup_chars[cur_idx] : '\0'
                );
            cur_idx++;
        }
    }
    return cur_idx;
}

class pocket_selector : public cataimgui::list_selector
{
        item *drag_drop_source;
        std::vector<item_pocket *> drag_drop_pockets;
        cataimgui::message_box *msg_box;
        int base_move_cost;
    public:
        pocket_selector( int base_move_cost, item *source,
                         std::vector<item_pocket *> pockets ) : list_selector( "Pocket Selector" ) {
            drag_drop_source = source;
            drag_drop_pockets.assign( pockets.begin(), pockets.end() );
            for( item_pocket *pocket : pockets ) {
                std::string pocket_text = string_format( _( "%s - %s/%s | %d moves" ),
                                          pocket->get_description().translated(), vol_to_info( "", "", pocket->contains_volume() ).sValue,
                                          vol_to_info( "", "", pocket->max_contains_volume() ).sValue, pocket->moves() + base_move_cost );
                ret_val<item_pocket::contain_code> contain = pocket->can_contain( *drag_drop_source );
                items.push_back( { pocket_text, contain.success(), false } );
            }
        }

        int get_base_move_cost() {
            return base_move_cost;
        }

        item *get_source() {
            return drag_drop_source;
        }

        std::vector<item_pocket *> get_pockets() {
            return drag_drop_pockets;
        }

        item *get_item() {
            return drag_drop_source;
        }

        item_pocket *get_selected_pocket() {
            int idx = get_selected_index();
            if( idx >= 0 && idx <= int( drag_drop_pockets.size() ) ) {
                return drag_drop_pockets[idx];
            }
            return nullptr;
        }

        bool any_pocket_enabled() {
            for( cataimgui::list_selector::litem &it : items ) {
                if( it.is_enabled ) {
                    return true;
                }
            }
            return false;
        }
};

inventory_entry &inventory_selector::draw_column( inventory_column *column, bool force_collate )
{
    const std::string &hl_option = get_option<std::string>( "INVENTORY_HIGHLIGHT" );
    static inventory_entry dummy( nullptr );
    inventory_entry &ent = dummy;
    for( inventory_entry &entry : column->entries ) {
        const inventory_entry::entry_cell_cache_t &cache = entry.get_entry_cell_cache( preset );
        ImGui::PushID( &entry );
        int indent = column->get_entry_indent( entry );
        ImGui::Indent( indent );
        if( entry.chevron ) {
            bool const hide_override = column->hide_entries_override && entry.any_item()->is_container();
            nc_color const col = entry.is_collation_header() ? c_light_blue : hide_override ?
                                 *column->hide_entries_override ? c_red : c_green : c_dark_gray;
            bool const stat = entry.is_collation_entry() ||
                              !hide_override ? entry.collapsed : *column->hide_entries_override;
            ImGui::Text( "%s", stat ? "▶" : "▼" );
            ImGui::SameLine();
        }
        bool tmp_selected = entry.chosen_count > 0;
        if( entry.get_invlet() ) {
            ImGui::Text( "%c", '[' );
            ImGui::SameLine( 0, 0 );
            draw_colored_text( string_format( "%c", entry.get_invlet() ), entry.get_invlet_color() );
            ImGui::SameLine( 0, 0 );
            ImGui::Text( "%c", ']' );
            ImGui::SameLine();
        }
        float text_width = ImGui::GetContentRegionAvail().x;
        if( !cache.text.empty() ) {
            auto orig_cpos = ImGui::GetCursorPos();
            float current_xpos = ImGui::GetContentRegionAvail().x + orig_cpos.x;
            for( size_t index = cache.text.size() - 1; index >= 1; index-- ) {
                if( column->cells[index].current_width == 0 ) {
                    column->calculate_cell_width( index );
                }
                current_xpos -= ( column->cells[index].current_width );

                ImGui::SetCursorPos( { current_xpos, orig_cpos.y } );
                draw_colored_text( cache.text[index], c_light_gray );
                text_width = current_xpos - orig_cpos.x;
            }
            ImGui::SetCursorPos( orig_cpos );
        }
        if( entry.is_item() && entry_to_be_focused == &entry.any_item() ) {
            ImGui::SetKeyboardFocusHere( 0 );
            entry_to_be_focused = nullptr;
        }
        std::string text = cache.text[0];
        nc_color color = cache.color;
        bool *selectable = &tmp_selected;
        if( entry.is_item() && !entry.is_selectable() ) {
            text = remove_color_tags( text );
            color = c_dark_gray;
            selectable = nullptr;
        } else if( entry.is_item() && entry.highlight_as_parent ) {
            if( hl_option == "symbol" ) {
                draw_colored_text( "<", h_white );
                ImGui::SameLine( 0, 0 );
            } else {
                text = remove_color_tags( cache.text[0] );
                color = c_white_white;
            }
        } else if( entry.is_item() && entry.highlight_as_child ) {
            if( hl_option == "symbol" ) {
                draw_colored_text( ">", h_white );
                ImGui::SameLine( 0, 0 );
            } else {
                text = remove_color_tags( cache.text[0] );
                color = c_black_white;
            }
        } else if( entry.is_category() || ( entry.denial.has_value() && !entry.denial->empty() ) ) {
            selectable = nullptr;
        }

        if( drag_enabled && selectable != nullptr ) {
            // this empty object allows the drag-drop logic to work, without this it crashes and burns.
            ImGui::Selectable( "", selectable );
            ImGui::SameLine( 0, 0 );
            if( ImGui::BeginDragDropSource() ) {
                ImGui::SetDragDropPayload( "INVENTORY_ENTRY", &entry, sizeof( inventory_entry ) );
                ImGui::Text( "%s", remove_color_tags( cache.text[0] ).c_str() );
                ImGui::EndDragDropSource();
            }
            if( entry.chevron ) {
                if( ImGui::BeginDragDropTarget() ) {
                    if( const ImGuiPayload *payload = ImGui::AcceptDragDropPayload( "INVENTORY_ENTRY" ) ) {
                        inventory_entry *source_entry = static_cast<inventory_entry *>( payload->Data );
                        drag_drop_item( source_entry->locations.back().get_item(),
                                        entry.locations.back().get_item() );
                    }
                    ImGui::EndDragDropTarget();
                }
            }
        }
        draw_colored_text( text, color, cataimgui::text_align::Left, text_width, selectable );
        if( ImGui::IsItemFocused() ) {
            keyboard_focused_entry = &entry;
            ent = entry;
        }
        if( ImGui::IsItemHovered( ImGuiHoveredFlags_NoNavOverride ) ) {
            mouse_hovered_entry = &entry;
        }

        ImGui::Unindent( indent );
        ImGui::PopID();
    }

    return ent;
}

size_t inventory_column::visible_cells() const
{
    return std::count_if( cells.begin(), cells.end(), []( const cell_t &elem ) {
        return elem.visible();
    } );
}

selection_column::selection_column( inventory_selector *parent, const std::string &id,
                                    const std::string &name ) :
    inventory_column( parent, selection_preset )
    , selected_cat( id, no_translation( name ), 0 )
{
    hide_entries_override = { false };
}

selection_column::~selection_column() = default;

void selection_column::prepare_paging( const std::string & )
{
    // always show all selected items
    inventory_column::prepare_paging( std::string() );

    if( entries.empty() ) { // Category must always persist
        entries.emplace_back( &*selected_cat );
    }

    if( !last_changed.is_null() ) {
        const auto iter = std::find( entries.begin(), entries.end(), last_changed );
        if( iter != entries.end() ) {
            //highlight( *iter, true );
        }
        last_changed = inventory_entry();
    }
}

void selection_column::on_change( const inventory_entry &entry )
{
    inventory_entry my_entry( entry, &*selected_cat );

    auto iter = std::find( entries.begin(), entries.end(), my_entry );

    if( iter == entries.end() ) {
        if( my_entry.chosen_count == 0 ) {
            return; // Not interested.
        }
        add_entry( my_entry )->make_entry_cell_cache( preset );
        paging_is_valid = false;
        last_changed = my_entry;
    } else if( iter->chosen_count != my_entry.chosen_count ) {
        if( my_entry.chosen_count > 0 ) {
            iter->chosen_count = my_entry.chosen_count;
            iter->make_entry_cell_cache( preset );
        } else {
            iter = entries.erase( iter );
        }
        paging_is_valid = false;
        if( iter != entries.end() ) {
            last_changed = *iter;
        }
    }
}
const item_category *inventory_selector::naturalize_category( const item_category &category,
        const tripoint &pos )
{
    const auto find_cat_by_id = [ this ]( const item_category_id & id ) {
        const auto iter = std::find_if( categories.begin(),
        categories.end(), [ &id ]( const item_category & cat ) {
            return cat.get_id() == id;
        } );
        return iter != categories.end() ? &*iter : nullptr;
    };

    const int dist = rl_dist( u.pos(), pos );

    if( dist != 0 ) {
        const std::string suffix = direction_suffix( u.pos(), pos );
        const item_category_id id = item_category_id( string_format( "%s_%s", category.get_id().c_str(),
                                    suffix.c_str() ) );

        const auto *existing = find_cat_by_id( id );
        if( existing != nullptr ) {
            return existing;
        }

        const std::string name = string_format( "%s %s", category.name(), suffix.c_str() );
        const int sort_rank = category.sort_rank() + dist;
        const item_category new_category( id, no_translation( name ), sort_rank );

        categories.push_back( new_category );
    } else {
        const item_category *const existing = find_cat_by_id( category.get_id() );
        if( existing != nullptr ) {
            return existing;
        }

        categories.push_back( category );
    }

    return &categories.back();
}

inventory_entry *inventory_selector::add_entry( inventory_column &target_column,
        std::vector<item_location> &&locations,
        const item_category *custom_category,
        const size_t chosen_count, item_location const &topmost_parent,
        bool chevron )
{
    if( !preset.is_shown( locations.front() ) ) {
        return nullptr;
    }

    is_empty = false;
    inventory_entry entry( locations, custom_category,
                           true, chosen_count,
                           entry_generation_number++, topmost_parent, chevron );

    entry.collapsed = locations.front()->is_collapsed();
    inventory_entry *const ret = target_column.add_entry( entry );

    return ret;
}

bool inventory_selector::add_entry_rec( inventory_column &entry_column,
                                        inventory_column &children_column, item_location &loc,
                                        item_category const *entry_category,
                                        item_category const *children_category,
                                        item_location const &topmost_parent, int indent )
{
    inventory_column temp_children( this, preset );
    bool vis_contents =
        add_contained_items( loc, temp_children, children_category,
                             get_topmost_parent( topmost_parent, loc, preset ),
                             preset.is_shown( loc ) ? indent + 2 : indent );
    inventory_entry *const nentry = add_entry( entry_column, std::vector<item_location>( 1, loc ),
                                    entry_category, 0, topmost_parent );
    if( nentry != nullptr ) {
        nentry->chevron = vis_contents;
        nentry->indent = indent;
        vis_contents = true;
    }
    temp_children.move_entries_to( children_column );

    return vis_contents;
}

bool inventory_selector::drag_drop_item( item *sourceItem, item *destItem )
{
    if( sourceItem == destItem ) {
        return false;
    }
    auto pockets = destItem->get_all_contained_pockets();
    if( pockets.empty() ) {

        show_popup_async( new cataimgui::message_box( _( "Error" ), _( "Destination has no pockets." ) ) );
        return false;
    }
    int base_move_cost = 0;
    auto parentContainers = u.parents( *sourceItem );
    for( item *parent : parentContainers ) {
        base_move_cost += parent->obtain_cost( *sourceItem );
    }
    pocket_selector *pocket_picker = new pocket_selector( base_move_cost, sourceItem, pockets );
    pocket_picker->set_draw_callback( [this, pocket_picker]() {
        if( pocket_picker->get_result() == cataimgui::dialog_result::OKClicked ) {
            item_pocket *pkt = pocket_picker->get_selected_pocket();
            auto contains = pkt->can_contain( *pocket_picker->get_item() );
            if( contains.success() ) {
                u.store( pkt, *pocket_picker->get_item(), true, pocket_picker->get_base_move_cost() );

                clear_items();
                add_character_items( u );
            } else {
                show_popup_async( new cataimgui::message_box( _( "Error" ), contains.str() ) );
            }
            pocket_picker->close();
        }
        return true;
    } );
    if( !pocket_picker->any_pocket_enabled() ) {
        delete pocket_picker;
        show_popup_async( new cataimgui::message_box( _( "Error" ), string_format(
                              _( "%s contains no pockets that can contain item." ),
                              destItem->tname( 1, false, 0, false, false, false ) ) ) );
        return false;
    }
    show_popup_async( pocket_picker );
    return true;
}


bool inventory_selector::add_contained_items( item_location &container )
{
    return add_contained_items( container, own_inv_column );
}

bool inventory_selector::add_contained_items( item_location &container, inventory_column &column,
        const item_category *const custom_category, item_location const &topmost_parent, int indent )
{
    if( container->has_flag( STATIC( flag_id( "NO_UNLOAD" ) ) ) ) {
        return false;
    }

    std::list<item *> const items = preset.get_pocket_type() == item_pocket::pocket_type::LAST
                                    ? container->all_items_top()
                                    : container->all_items_top( preset.get_pocket_type() );

    bool vis_top = false;
    inventory_column temp( this, preset );
    for( item *it : items ) {
        item_location child( container, it );
        item_category const *hacked_cat = custom_category;
        inventory_column *hacked_col = &column;
        if( is_worn_ablative( container, child ) ) {
            hacked_cat = &item_category_ITEMS_WORN.obj();
            hacked_col = &own_gear_column;
        }
        vis_top |= add_entry_rec( *hacked_col, temp, child, hacked_cat, custom_category,
                                  topmost_parent, indent );
    }
    temp.move_entries_to( column );
    return vis_top;
}

void inventory_selector::add_contained_ebooks( item_location &container )
{
    if( !container->is_ebook_storage() ) {
        return;
    }

    for( item *it : container->get_contents().ebooks() ) {
        item_location child( container, it );
        add_entry( own_inv_column, std::vector<item_location>( 1, child ) );
    }
}

void inventory_selector::add_character_items( Character &character )
{
    item_location weapon = character.get_wielded_item();
    bool const hierarchy = _uimode == uimode::hierarchy;
    if( weapon ) {
        add_entry_rec( own_gear_column, hierarchy ? own_gear_column : own_inv_column, weapon,
                       &item_category_WEAPON_HELD.obj(),
                       hierarchy ? &item_category_WEAPON_HELD.obj() : nullptr );
    }
    for( item_location &worn_item : character.top_items_loc() ) {
        item_category const *const custom_cat = wielded_worn_category( worn_item, u );
        add_entry_rec( own_gear_column, hierarchy ? own_gear_column : own_inv_column, worn_item,
                       custom_cat, hierarchy ? custom_cat : nullptr );
    }
    if( !hierarchy ) {
        own_inv_column.set_indent_entries_override( false );
    }
}

void inventory_selector::add_map_items( const tripoint &target )
{
    map &here = get_map();
    if( here.accessible_items( target ) ) {
        map_stack items = here.i_at( target );
        const std::string name = to_upper_case( here.name( target ) );
        const item_category map_cat( name, no_translation( name ), 100 );
        _add_map_items( target, map_cat, items, [target]( item & it ) {
            return item_location( map_cursor( target ), &it );
        } );
    }
}

void inventory_selector::add_vehicle_items( const tripoint &target )
{
    const std::optional<vpart_reference> vp =
        get_map().veh_at( target ).part_with_feature( "CARGO", true );
    if( !vp ) {
        return;
    }
    vehicle *const veh = &vp->vehicle();
    const int part = vp->part_index();
    vehicle_stack items = veh->get_items( part );
    const std::string name = to_upper_case( remove_color_tags( veh->part( part ).name() ) );
    const item_category vehicle_cat( name, no_translation( name ), 200 );
    _add_map_items( target, vehicle_cat, items, [veh, part]( item & it ) {
        return item_location( vehicle_cursor( *veh, part ), &it );
    } );
}

void inventory_selector::_add_map_items( tripoint const &target, item_category const &cat,
        item_stack &items, std::function<item_location( item & )> const &floc )
{
    bool const hierarchy = _uimode == uimode::hierarchy;
    item_category const *const custom_cat = hierarchy ? naturalize_category( cat, target ) : nullptr;
    inventory_column *const col = _categorize_map_items ? &own_inv_column : &map_column;

    inventory_column temp( this, preset );
    inventory_column temp_cont( this, preset );
    for( item &it : items ) {
        item_location loc = floc( it );
        add_entry_rec( temp, temp_cont, loc, custom_cat, custom_cat );
    }

    temp.move_entries_to( *col );
    temp_cont.move_entries_to( *col );

    if( !hierarchy ) {
        col->set_indent_entries_override( false );
    }
}

void inventory_selector::add_nearby_items( int radius )
{
    if( radius >= 0 ) {
        map &here = get_map();
        for( const tripoint &pos : closest_points_first( u.pos(), radius ) ) {
            // can not reach this -> can not access its contents
            if( u.pos() != pos && !here.clear_path( u.pos(), pos, rl_dist( u.pos(), pos ), 1, 100 ) ) {
                continue;
            }
            add_map_items( pos );
            add_vehicle_items( pos );
        }
    }
}

void inventory_selector::add_remote_map_items( tinymap *remote_map, const tripoint &target )
{
    map_stack items = remote_map->i_at( target );
    const std::string name = to_upper_case( remote_map->name( target ) );
    const item_category map_cat( name, no_translation( name ), 100 );
    _add_map_items( target, map_cat, items, [target]( item & it ) {
        return item_location( map_cursor( target ), &it );
    } );
}

void inventory_selector::clear_items()
{
    is_empty = true;
    for( inventory_column *&column : columns ) {
        column->clear();
    }
    own_inv_column.clear();
    own_gear_column.clear();
    map_column.clear();
}

bool inventory_selector::highlight( const item_location &loc, bool hidden, bool front_only )
{
    bool res = false;

    for( size_t i = 0; i < columns.size(); ++i ) {
        inventory_column *elem = columns[i];
        if( !elem->visible() ) {
            continue;
        }
        bool found = false;
        if( hidden ) {
            if( inventory_entry *ent = elem->find_by_location( loc, true ) ) {
                item_location const &parent = ent->is_collation_entry() ? ent->collation_meta->tip :
                                              ent->topmost_parent;
                if( !parent ) {
                    continue;
                }
                inventory_entry *pent = elem->find_by_location( parent );
                if( elem != nullptr ) {
                    elem->set_collapsed( *pent, false );
                    prepare_layout();
                    found = elem->highlight( loc, true );
                }
            }
        } else {
            found = elem->highlight( loc, front_only );
        }
        if( found && !res && elem->activatable() ) {
            set_active_column( i );
            res = true;
        }
    }

    return res;
}

item_location inventory_selector::get_collation_next() const
{
    inventory_column *col = columns[active_column_index];
    inventory_entry &ent = col->get_highlighted();
    if( !ent.is_collated() ) {
        return {};
    }
    if( ent.is_collation_header() ) {
        return ent.locations.front();
    }

    item_location const &loc_cur = ent.locations.front();
    inventory_entry *p_ent = col->find_by_location( ent.collation_meta->tip );
    if( p_ent ) {
        auto iter = std::find( p_ent->locations.begin(), p_ent->locations.end(), loc_cur );
        if( iter == --p_ent->locations.end() ) {
            return *--iter;
        }
        return *++iter;
    }
    return item_location();
}

bool inventory_selector::highlight_one_of( const std::vector<item_location> &locations,
        bool hidden )
{
    prepare_layout();
    for( const item_location &loc : locations ) {
        if( loc && highlight( loc, hidden ) ) {
            return true;
        }
    }
    return false;
}

inventory_entry *inventory_selector::find_entry_by_invlet( int invlet ) const
{
    for( const inventory_column *elem : columns ) {
        inventory_entry *const res = elem->find_by_invlet( invlet );
        if( res != nullptr ) {
            return res;
        }
    }
    return nullptr;
}

inventory_entry *inventory_selector::find_entry_by_coordinate( const point &coordinate ) const
{
    for( auto pair : rect_entry_map ) {
        if( pair.first.contains( coordinate ) ) {
            return pair.second;
        }
    }
    return nullptr;
}

inventory_entry *inventory_selector::find_entry_by_location( item_location &loc ) const
{
    for( const inventory_column *elem : columns ) {
        if( elem->allows_selecting() ) {
            inventory_entry *const res = elem->find_by_location( loc );
            if( res != nullptr ) {
                return res;
            }
        }
    }
    return nullptr;
}

// FIXME: if columns are merged due to low screen width, they will not be splitted
// once screen width becomes enough for the columns.
void inventory_selector::rearrange_columns( size_t client_width )
{
    const inventory_entry &prev_entry = get_highlighted();
    const item_location prev_selection = prev_entry.is_item() ?
                                         prev_entry.any_item() : item_location::nowhere;
    bool const front_only = prev_entry.is_collation_entry();
    size_t max_width = 0;
    for( auto column : get_visible_columns() ) {
        for( size_t index = 0; index < column->cells.size(); index++ ) {
            column->calculate_cell_width( index );
        }
        max_width = std::max( max_width, column->get_cells_width() );
    }
    // does one column want to take up >50% of our window width? if so we've got problems
    bool is_overflowed = ( float( max_width ) / client_width ) > 0.6;
    if( is_overflowed && !own_gear_column.empty() ) {
        if( own_inv_column.empty() ) {
            own_inv_column.set_indent_entries_override( own_gear_column.indent_entries() );
        }
        own_gear_column.move_entries_to( own_inv_column );
    }
    if( is_overflowed && !map_column.empty() ) {
        if( own_inv_column.empty() ) {
            own_inv_column.set_indent_entries_override( map_column.indent_entries() );
        }
        map_column.move_entries_to( own_inv_column );
    }
    if( prev_selection ) {
        //highlight( prev_selection, false, front_only );
    }
}

void inventory_selector::reassign_custom_invlets()
{
    if( invlet_type_ == SELECTOR_INVLET_DEFAULT || invlet_type_ == SELECTOR_INVLET_NUMERIC ) {
        int min_invlet = static_cast<uint8_t>( use_invlet ? '0' : '\0' );
        for( inventory_column *elem : columns ) {
            elem->prepare_paging();
            min_invlet = elem->reassign_custom_invlets( u, min_invlet, use_invlet ? '9' : '\0' );
        }
    } else if( invlet_type_ == SELECTOR_INVLET_ALPHA ) {
        const std::string all_pickup_chars = use_invlet ?
                                             "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ:;" : "";
        std::string pickup_chars = ctxt.get_available_single_char_hotkeys( all_pickup_chars );
        int cur_idx = 0;
        auto elemfilter = []( const inventory_entry & e ) {
            return e.is_item() && e.any_item()->invlet > '\0';
        };
        // First pass -> remove letters taken by user-set invlets
        for( inventory_column *elem : columns ) {
            for( inventory_entry *e : elem->get_entries( elemfilter ) ) {
                const char c = e->any_item()->invlet;
                if( pickup_chars.find_first_of( c ) != std::string::npos ) {
                    pickup_chars.erase( std::remove( pickup_chars.begin(), pickup_chars.end(), c ),
                                        pickup_chars.end() );
                }
            }
        }
        for( inventory_column *elem : columns ) {
            elem->prepare_paging();
            cur_idx = elem->reassign_custom_invlets( cur_idx, pickup_chars );
        }
    }
}

void inventory_selector::prepare_layout()
{
    startup_timer const tp_prep =
        std::chrono::time_point_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() );

    // This block adds categories and should go before any width evaluations
    for( inventory_column *&elem : columns ) {
        elem->prepare_paging( filter );
    }

    // Handle screen overflow
    rearrange_columns( size_t( get_bounds().w ) );

    reassign_custom_invlets();

    refresh_active_column();

    debug_print_timer( tp_prep, "prepare_layout took" );
}

void inventory_selector::on_deactivate()
{
    columns[active_column_index]->on_deactivate();
}

void inventory_selector::highlight_position( std::pair<size_t, size_t> position )
{
    prepare_layout();
    set_active_column( position.first );
    columns[active_column_index]->highlight( position.second, scroll_direction::BACKWARD );
}

static inventory_entry inv_empty;
inventory_entry &inventory_selector::get_highlighted()
{
    //for( inventory_column *column : columns ) {
    //    inventory_entry &entry = column->get_highlighted();
    //    if( entry ) {
    //        return entry;
    //    }
    //}

    if( mouse_hovered_entry != nullptr ) {
        return *mouse_hovered_entry;
    } else if( keyboard_focused_entry != nullptr ) {
        return *keyboard_focused_entry;
    }
    return inv_empty;
}

std::vector<inventory_entry *> inventory_selector::get_all_highlighted()
{
    std::vector<inventory_entry *> output;
    for( inventory_column *column : columns ) {
        std::vector<inventory_entry *> tmp = column->get_all_selected();
        output.insert( output.end(), tmp.begin(), tmp.end() );
    }
    return output;
}

size_t inventory_selector::get_header_height() const
{
    return display_stats || !hint.empty()
           ? std::max<size_t>( 3, 2 + std::count( hint.begin(), hint.end(), '\n' ) )
           : 1;
}

inventory_selector::stat display_stat( const std::string &caption, int cur_value, int max_value,
                                       const std::function<std::string( int )> &disp_func )
{
    const nc_color color = cur_value > max_value ? c_red : c_light_gray;
    return {{
            caption,
            colorize( disp_func( cur_value ), color ), "/",
            colorize( disp_func( max_value ), c_light_gray )
        }};
}

inventory_selector::stat inventory_selector::get_weight_and_length_stat(
    units::mass weight_carried, units::mass weight_capacity,
    const units::length &longest_length )
{
    std::string length_weight_caption = string_format( _( "Longest Length (%s): %s Weight (%s):" ),
                                        length_units( longest_length ), colorize( std::to_string( convert_length( longest_length ) ),
                                                c_light_gray ), weight_units() );
    return display_stat( length_weight_caption, to_gram( weight_carried ),
    to_gram( weight_capacity ), []( int w ) {
        return string_format( "%.1f", round_up( convert_weight( units::from_gram( w ) ), 1 ) );
    } );
}
inventory_selector::stat inventory_selector::get_volume_stat( const units::volume
        &volume_carried, const units::volume &volume_capacity, const units::volume &largest_free_volume )
{
    std::string volume_caption = string_format( _( "Free Volume (%s): %s Volume (%s):" ),
                                 volume_units_abbr(),
                                 colorize( format_volume( largest_free_volume ), c_light_gray ),
                                 volume_units_abbr() );
    return display_stat( volume_caption, units::to_milliliter( volume_carried ),
    units::to_milliliter( volume_capacity ), []( int v ) {
        return format_volume( units::from_milliliter( v ) );
    } );
}
inventory_selector::stat inventory_selector::get_holster_stat( const units::volume
        &holster_volume, int used_holsters, int total_holsters )
{

    std::string holster_caption = string_format( _( "Free Holster Volume (%s): %s Used Holsters:" ),
                                  volume_units_abbr(),
                                  colorize( format_volume( holster_volume ), c_light_gray ) );


    return display_stat( holster_caption, used_holsters, total_holsters, []( int v ) {
        return string_format( "%d", v );
    } );
}

inventory_selector::stats inventory_selector::get_weight_and_volume_and_holster_stats(
    units::mass weight_carried, units::mass weight_capacity,
    const units::volume &volume_carried, const units::volume &volume_capacity,
    const units::length &longest_length, const units::volume &largest_free_volume,
    const units::volume &holster_volume, int used_holsters, int total_holsters )
{
    return { {
            get_weight_and_length_stat( weight_carried, weight_capacity, longest_length ),
            get_volume_stat( volume_carried, volume_capacity, largest_free_volume ),
            get_holster_stat( holster_volume, used_holsters, total_holsters )
        }
    };
}

inventory_selector::stats inventory_selector::get_raw_stats() const
{
    return get_weight_and_volume_and_holster_stats( u.weight_carried(), u.weight_capacity(),
            u.volume_carried(), u.volume_capacity(),
            u.max_single_item_length(), u.max_single_item_volume(),
            u.free_holster_volume(), u.used_holsters(), u.total_holsters() );
}

std::vector<std::string> inventory_selector::get_stats() const
{
    // Stats consist of arrays of cells.
    const size_t num_stats = 3;
    const std::array<stat, num_stats> stats = get_raw_stats();
    // Streams for every stat.
    std::array<std::string, num_stats> lines;
    std::array<size_t, num_stats> widths;
    // Add first cells and spaces after them.
    for( size_t i = 0; i < stats.size(); ++i ) {
        lines[i] += string_format( "%s", stats[i][0] ) + " ";
    }
    // Now add the rest of the cells and align them to the right.
    for( size_t j = 1; j < stats.front().size(); ++j ) {
        // Calculate actual cell width for each stat.
        std::transform( stats.begin(), stats.end(), widths.begin(),
        [j]( const stat & elem ) {
            return utf8_width( elem[j], true );
        } );
        // Determine the max width.
        const size_t max_w = *std::max_element( widths.begin(), widths.end() );
        // Align all stats in this cell with spaces.
        for( size_t i = 0; i < stats.size(); ++i ) {
            if( max_w > widths[i] ) {
                lines[i] += std::string( max_w - widths[i], ' ' );
            }
            lines[i] += string_format( "%s", stats[i][j] );
        }
    }
    // Construct the final result.
    return std::vector<std::string>( lines.begin(), lines.end() );
}

std::pair< bool, std::string > inventory_selector::query_string( const std::string &val,
        bool end_with_toggle )
{
    string_input_popup spopup;
    spopup.max_length( 256 ).text( val );
    if( end_with_toggle ) {
        for( input_event const &iev : inp_mngr.get_input_for_action( "TOGGLE_ENTRY", "INVENTORY" ) ) {
            spopup.add_callback( iev.get_first_input(), [&spopup]() {
                spopup.confirm();
                return true;
            } );
        }
    };

    //current_ui->mark_resize();

    do {
        ui_manager::redraw();
        spopup.query_string( /*loop=*/false );
    } while( !spopup.confirmed() && !spopup.canceled() );

    std::string rval;
    bool confirmed = spopup.confirmed();
    if( confirmed ) {
        rval = spopup.text();
    }

    return std::make_pair( confirmed, rval );
}

int inventory_selector::query_count( char init, bool end_with_toggle )
{
    std::string sinit = init != 0 ? std::string( 1, init ) : std::string();
    std::pair< bool, std::string > query = query_string( sinit, end_with_toggle );
    int ret = -1;
    if( query.first ) {
        try {
            ret = std::stoi( query.second );
        } catch( const std::invalid_argument &e ) {
            // TODO Tell User they did a bad
            ret = -1;
        } catch( const std::out_of_range &e ) {
            ret = INT_MAX;
        }
    }

    return ret;
}

void inventory_selector::set_filter( const std::string &str )
{
    filter = str;
    for( inventory_column *const elem : columns ) {
        elem->set_filter( filter );
    }
}

std::string inventory_selector::get_filter() const
{
    return filter;
}

std::pair<std::string, nc_color> inventory_selector::get_footer( navigation_mode m ) const
{
    if( has_available_choices() ) {
        return std::make_pair( get_navigation_data( m ).name.translated(),
                               get_navigation_data( m ).color );
    }
    return std::make_pair( _( "There are no available choices" ), i_red );
}
inventory_selector::inventory_selector( cataimgui::window *parent, Character &u,
                                        const inventory_selector_preset &preset )
    : cataimgui::window( parent, ImGuiWindowFlags_AlwaysAutoResize )
    , u( u )
    , preset( preset )
    , multiselect( false )
    , ctxt( "INVENTORY", keyboard_mode::keychar )
    , drag_enabled( false )
    , active_column_index( 0 )
    , mode( navigation_mode::ITEM )
    , own_inv_column( this, preset )
    , own_gear_column( this, preset )
    , map_column( this, preset )
    , _uimode( preset.save_state == nullptr ? inventory_sel_default_state.uimode :
               preset.save_state->uimode )
{
    set_title( "Inventory" );
    item_name_cache_users++;
    tp_start =
        std::chrono::time_point_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() );
    ctxt.register_action( "CONFIRM", to_translation( "Confirm your selection" ) );
    ctxt.register_action( "QUIT", to_translation( "Cancel" ) );
    ctxt.register_action( "CATEGORY_SELECTION", to_translation( "Switch category selection mode" ) );
    ctxt.register_action( "TOGGLE_FAVORITE", to_translation( "Toggle favorite" ) );
    ctxt.register_action( "SELECT" );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "VIEW_CATEGORY_MODE" );
    ctxt.register_action( "TOGGLE_NUMPAD_NAVIGATION" );
    ctxt.register_action( "ANY_INPUT" ); // For invlets
    ctxt.register_action( "INVENTORY_FILTER" );
    ctxt.register_action( "RESET_FILTER" );
    ctxt.register_action( "EXAMINE" );
    ctxt.register_action( "SHOW_HIDE_CONTENTS", to_translation( "Show/hide contents" ) );
    ctxt.register_action( "SHOW_HIDE_CONTENTS_ALL" );
    ctxt.register_action( "EXAMINE_CONTENTS" );
    ctxt.register_action( "TOGGLE_SKIP_UNSELECTABLE" );
    ctxt.register_action( "ORGANIZE_MENU" );
    ctxt.set_timeout( 0 );

    append_column( own_inv_column );
    append_column( map_column );
    append_column( own_gear_column );

    for( inventory_column *column : columns ) {
        column->toggle_skip_unselectable( skip_unselectable );
    }
    mouse_hovered_entry = nullptr;
    keyboard_focused_entry = nullptr;
}

inventory_selector::inventory_selector( Character &u, const inventory_selector_preset &preset )
    : cataimgui::window( "Inventory", ImGuiWindowFlags_AlwaysAutoResize )
    , u( u )
    , preset( preset )
    , multiselect( false )
    , ctxt( "INVENTORY", keyboard_mode::keychar )
    , drag_enabled( false )
    , active_column_index( 0 )
    , mode( navigation_mode::ITEM )
    , own_inv_column( this, preset )
    , own_gear_column( this, preset )
    , map_column( this, preset )
    , _uimode( preset.save_state == nullptr ? inventory_sel_default_state.uimode :
               preset.save_state->uimode )
{
    item_name_cache_users++;
    tp_start =
        std::chrono::time_point_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now() );
    ctxt.register_action( "CONFIRM", to_translation( "Confirm your selection" ) );
    ctxt.register_action( "QUIT", to_translation( "Cancel" ) );
    ctxt.register_action( "CATEGORY_SELECTION", to_translation( "Switch category selection mode" ) );
    ctxt.register_action( "TOGGLE_FAVORITE", to_translation( "Toggle favorite" ) );
    ctxt.register_action( "SELECT" );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "VIEW_CATEGORY_MODE" );
    ctxt.register_action( "TOGGLE_NUMPAD_NAVIGATION" );
    ctxt.register_action( "ANY_INPUT" ); // For invlets
    ctxt.register_action( "INVENTORY_FILTER" );
    ctxt.register_action( "RESET_FILTER" );
    ctxt.register_action( "EXAMINE" );
    ctxt.register_action( "SHOW_HIDE_CONTENTS", to_translation( "Show/hide contents" ) );
    ctxt.register_action( "SHOW_HIDE_CONTENTS_ALL" );
    ctxt.register_action( "EXAMINE_CONTENTS" );
    ctxt.register_action( "TOGGLE_SKIP_UNSELECTABLE" );
    ctxt.register_action( "ORGANIZE_MENU" );
    ctxt.set_timeout( 0 );

    append_column( own_inv_column );
    append_column( map_column );
    append_column( own_gear_column );

    for( inventory_column *column : columns ) {
        column->toggle_skip_unselectable( skip_unselectable );
    }
    mouse_hovered_entry = nullptr;
    keyboard_focused_entry = nullptr;
}

inventory_selector::~inventory_selector()
{
    ctxt.reset_timeout();
    item_name_cache_users--;
    if( item_name_cache_users <= 0 ) {
        item_name_cache.clear();
    }
    if( preset.save_state == nullptr ) {
        inventory_sel_default_state.uimode = _uimode;
    } else {
        preset.save_state->uimode = _uimode;
    }
}

bool inventory_selector::empty() const
{
    return is_empty;
}

bool inventory_selector::has_available_choices() const
{
    return std::any_of( columns.begin(), columns.end(), []( const inventory_column * element ) {
        return element->has_available_choices();
    } );
}

inventory_input inventory_selector::get_input()
{
    mouse_hovered_entry = nullptr;
    keyboard_focused_entry = nullptr;
    std::string const &action = ctxt.handle_input();
    int const ch = ctxt.get_raw_input().get_first_input();
    return process_input( action, ch );
}

inventory_input inventory_selector::process_input( const std::string &action, int ch )
{
    inventory_input res{ action, ch, nullptr };

    res.entry = find_entry_by_invlet( res.ch );
    if( res.entry != nullptr && !res.entry->is_selectable() ) {
        res.entry = nullptr;
    }
    if( res.entry == nullptr ) {
        inventory_entry &tmp = get_highlighted();
        if( tmp ) {
            res.entry = &tmp;
        }
    }
    return res;
}

void inventory_column::cycle_hide_override()
{
    if( hide_entries_override ) {
        if( *hide_entries_override ) {
            hide_entries_override = std::nullopt;
        } else {
            hide_entries_override = true;
        }
    } else {
        hide_entries_override = false;
    }
}

void selection_column::cycle_hide_override()
{
    // never hide entries
}

void inventory_selector::on_input( const inventory_input &input )
{
    if( input.action == "CATEGORY_SELECTION" ) {
        toggle_navigation_mode();
    } else if( input.action == "VIEW_CATEGORY_MODE" ) {
        toggle_categorize_contained();
    } else if( input.action == "EXAMINE_CONTENTS" ) {
        const inventory_entry &selected = get_highlighted();
        if( selected ) {
            //TODO: Should probably be any_item() rather than direct f?ront() access, but that seems to lock us into const item_location, which various functions are unprepared for
            item_location sitem = selected.locations.front();
            inventory_examiner examine_contents( u, sitem );
            examine_contents.add_contained_items( sitem );
            int examine_result = examine_contents.execute();
            if( examine_result == EXAMINED_CONTENTS_WITH_CHANGES ) {
                //The user changed something while examining, so rebuild paging
                for( inventory_column *elem : columns ) {
                    elem->invalidate_paging();
                }
            } else if( examine_result == NO_CONTENTS_TO_EXAMINE ) {
                action_examine( sitem );
            }
        }
    } else if( input.action == "EXAMINE" ) {
        const inventory_entry &selected = get_highlighted();
        if( selected ) {
            const item_location &sitem = selected.any_item();
            action_examine( sitem );
        }
    } else if( input.action == "INVENTORY_FILTER" ) {
        std::shared_ptr<cataimgui::string_input_box> input_box( new
                cataimgui::string_input_box( "Set Filter",
                                             "Enter new inventory filter:" ) );
        if( show_popup( input_box ) == cataimgui::dialog_result::OKClicked ) {
            set_filter( input_box.get()->get_input() );
        }
    } else if( input.action == "RESET_FILTER" ) {
        set_filter( "" );
        //ui.lock()->mark_resize();
    } else if( input.action == "TOGGLE_SKIP_UNSELECTABLE" ) {
        toggle_skip_unselectable();
    } else {
        for( inventory_column *elem : columns ) {
            elem->on_input( input );
        }
        refresh_active_column(); // Columns can react to actions by losing their activation capacity
        if( input.action == "TOGGLE_FAVORITE" ) {
            // Favoriting items changes item name length which may require resizing
            //current_ui->mark_resize();
        }
        if( input.action == "SHOW_HIDE_CONTENTS_ALL" ) {
            for( inventory_column *col : columns ) {
                col->cycle_hide_override();
            }
        }
        if( input.action == "SHOW_HIDE_CONTENTS" || input.action == "SHOW_HIDE_CONTENTS_ALL" ) {
            for( inventory_column * const &col : columns ) {
                col->invalidate_paging();
            }
            std::vector<item_location> inv = get_highlighted().locations;
            //current_ui->mark_resize();
            highlight_one_of( inv );
        }
    }
}

void inventory_selector::on_change( const inventory_entry &entry )
{
    for( inventory_column *&elem : columns ) {
        elem->on_change( entry );
    }
    refresh_active_column(); // Columns can react to changes by losing their activation capacity
}

std::vector<inventory_column *> inventory_selector::get_visible_columns() const
{
    std::vector<inventory_column *> res( columns.size() );
    const auto iter = std::copy_if( columns.begin(), columns.end(), res.begin(),
    []( const inventory_column * e ) {
        return e->visible();
    } );
    res.resize( std::distance( res.begin(), iter ) );
    return res;
}

inventory_column &inventory_selector::get_column( size_t index )
{
    if( index >= columns.size() ) {
        static inventory_column dummy( this, preset );
        return dummy;
    }
    return *columns[index];
}

std::pair<size_t, size_t> inventory_selector::get_highlighted_position() const
{
    std::pair<size_t, size_t> position;
    for( size_t column_index = 0; column_index < columns.size(); column_index++ ) {
        size_t idx = columns[column_index]->get_highlighted_index();
        if( idx != SIZE_MAX ) {
            position.first = column_index;
            position.second = idx;
            break;
        }
    }
    return position;
}

void inventory_selector::set_active_column( size_t index )
{
    if( index < columns.size() && index != active_column_index && get_column( index ).activatable() ) {
        if( active_column_index != SIZE_MAX ) {
            columns[active_column_index]->on_deactivate();
        }
        active_column_index = index;
        if( active_column_index != SIZE_MAX ) {
            columns[active_column_index]->on_activate();
        }
    }
}

void inventory_selector::toggle_skip_unselectable()
{
    skip_unselectable = !skip_unselectable;
    for( inventory_column *col : columns ) {
        col->toggle_skip_unselectable( skip_unselectable );
    }
}

void inventory_selector::_categorize( inventory_column &col )
{
    // Remove custom category and allow entries to categorize by their item's category
    for( inventory_entry *entry : col.get_entries( return_item, true ) ) {
        const item_location loc = entry->any_item();
        // ensure top-level equipped entries don't lose their special categories
        const item_category *custom_category = wielded_worn_category( loc, u );

        entry->set_custom_category( custom_category );
    }
    col.set_indent_entries_override( false );
    col.invalidate_paging();
    col.uncollate();
}

void inventory_selector::_uncategorize( inventory_column &col )
{
    for( inventory_entry *entry : col.get_entries( return_item, true ) ) {
        // find the topmost parent of the entry's item and categorize it by that
        // to form the hierarchy
        item_location ancestor = entry->any_item();
        while( ancestor.has_parent() ) {
            ancestor = ancestor.parent_item();
        }

        const item_category *custom_category = nullptr;
        if( ancestor.where() != item_location::type::character ) {
            const std::string name = to_upper_case( remove_color_tags( ancestor.describe() ) );
            const item_category map_cat( name, no_translation( name ), 100 );
            custom_category = naturalize_category( map_cat, ancestor.position() );
        } else {
            custom_category = wielded_worn_category( ancestor, u );
        }

        entry->set_custom_category( custom_category );
    }
    col.clear_indent_entries_override();
    col.invalidate_paging();
    col.uncollate();
}

void inventory_selector::toggle_categorize_contained()
{
    std::vector<item_location> highlighted;
    if( get_highlighted().is_item() ) {
        highlighted = get_highlighted().locations;
    }

    if( _uimode == uimode::hierarchy ) {
        inventory_column replacement_column( this );

        // split entries into either worn/held gear or contained items
        for( inventory_entry *entry : own_gear_column.get_entries( return_item, true ) ) {
            const item_location loc = entry->any_item();
            inventory_column *col = is_container( loc ) && !is_worn_ablative( loc.parent_item(), loc ) ?
                                    &own_inv_column : &replacement_column;
            col->add_entry( *entry );
        }
        own_gear_column.clear();
        replacement_column.move_entries_to( own_gear_column );

        for( inventory_column *col : columns ) {
            _categorize( *col );
        }
        _uimode = uimode::categories;
    } else {
        // move all entries into one big gear column and turn into hierarchy
        own_inv_column.move_entries_to( own_gear_column );
        for( inventory_column *col : columns ) {
            _uncategorize( *col );
        }
        _uimode = uimode::hierarchy;
    }

    if( !highlighted.empty() ) {
        highlight_one_of( highlighted );
    }

    // needs to be called now so that new invlets can be assigned
    // and subclasses w/ selection columns can then re-populate entries
    // using the new invlets
    prepare_layout();

    // invalidate, but dont mark resize, to avoid re-calling prepare_layout()
    // and as a consequence reassign_custom_invlets()
    //invalidate_ui();
}

void inventory_selector::toggle_active_column( scroll_direction dir )
{
    if( columns.empty() ) {
        return;
    }

    size_t index = active_column_index;

    do {
        switch( dir ) {
            case scroll_direction::FORWARD:
                index = index + 1 < columns.size() ? index + 1 : 0;
                break;
            case scroll_direction::BACKWARD:
                index = index > 0 ? index - 1 : columns.size() - 1;
                break;
        }
    } while( index != active_column_index && !get_column( index ).activatable() );

    set_active_column( index );
}

void inventory_selector::toggle_navigation_mode()
{
    mode = get_navigation_data( mode ).next_mode;
    for( inventory_column *&elem : columns ) {
        elem->on_mode_change( mode );
    }
}

void inventory_selector::append_column( inventory_column &column )
{
    column.on_mode_change( mode );

    if( columns.empty() ) {
        column.on_activate();
    }

    columns.push_back( &column );
}

const navigation_mode_data &inventory_selector::get_navigation_data( navigation_mode m ) const
{
    static const std::map<navigation_mode, navigation_mode_data> mode_data = {
        { navigation_mode::ITEM,     { navigation_mode::CATEGORY, translation(),                               c_light_gray } },
        { navigation_mode::CATEGORY, { navigation_mode::ITEM,     to_translation( "Category selection mode" ), h_white  } }
    };

    return mode_data.at( m );
}

std::string inventory_selector::action_bound_to_key( char key ) const
{
    return ctxt.input_to_action( input_event( key, input_event_t::keyboard_char ) );
}

item_location inventory_pick_selector::execute()
{
    for( inventory_column *col : columns ) {
        col->prepare_paging();
    }
    debug_print_timer( tp_start );
    item_location startDragItem;
    while( true ) {
#if !(defined(TILES) || defined(WIN32))
        ui_adaptor::redraw_all_invalidated( true );
#endif
        const inventory_input input = get_input();

        if( !is_open || input.action == "QUIT" ) {
            return item_location();
        } else if( input.action == "SELECT" ) {
            if( mouse_hovered_entry != nullptr && mouse_hovered_entry->is_item() &&
                !cataimgui::is_drag_drop_active() ) {
                return mouse_hovered_entry->any_item();
            } else {
                on_input( input );
            }
        } else if( input.action == "ANY_INPUT" ) {
            if( input.ch != UNKNOWN_UNICODE ) {
                for( inventory_column *col : columns ) {
                    inventory_entry *ent = col->find_by_invlet( input.ch );
                    if( ent ) {
                        return ent->any_item();
                    }
                }
            }
        } else if( input.action == "ORGANIZE_MENU" ) {
            u.worn.organize_items_menu();
            return item_location();
        } else if( input.action == "CONFIRM" ) {
            inventory_entry &highlighted = get_highlighted();
            if( highlighted && highlighted.is_selectable() ) {
                return highlighted.any_item();
            }
        } else {
            on_input( input );
        }
    }
}

inventory_selector::stats container_inventory_selector::get_raw_stats() const
{
    return get_weight_and_volume_and_holster_stats( loc->get_total_contained_weight(),
            loc->get_total_weight_capacity(),
            loc->get_total_contained_volume(), loc->get_total_capacity(),
            loc->max_containable_length(), loc->max_containable_volume(),
            loc->get_total_holster_volume() - loc->get_used_holster_volume(),
            loc->get_used_holsters(), loc->get_total_holsters() );
}

void inventory_selector::action_examine( const item_location &sitem )
{
    // Code below pulled from the action_examine function in advanced_inv.cpp
    std::vector<iteminfo> vThisItem;
    std::vector<iteminfo> vDummy;

    sitem->info( true, vThisItem );
    vThisItem.insert( vThisItem.begin(),
    { {}, string_format( _( "Location: %s" ), sitem.describe( &u ) ) } );

    item_info_data data( sitem->tname(), sitem->type_name(), vThisItem, vDummy );
    data.handle_scrolling = true;
    draw_item_info( [&]() -> catacurses::window {
        int maxwidth = std::max( FULL_SCREEN_WIDTH, TERMX );
        int width = std::min( 80, maxwidth );
        return catacurses::newwin( 0, width, point( maxwidth / 2 - width / 2, 0 ) ); },
    data ).get_first_input();
}

void inventory_selector::highlight()
{
    const inventory_entry &selected = columns[active_column_index]->get_highlighted();
    if( !selected.is_item() ) {
        return;
    }
    item_location parent = item_location::nowhere;
    bool selected_has_parent = false;
    if( selected.is_item() && selected.any_item().has_parent() ) {
        parent = selected.any_item().parent_item();
        selected_has_parent = true;
    }
    for( const inventory_column *column : get_all_columns() ) {
        for( inventory_entry *entry : column->get_entries( return_item ) ) {
            // Find parent of selected.
            if( selected_has_parent ) {
                // Check if parent is in a stack.
                for( const item_location &test_loc : entry->locations ) {
                    if( test_loc == parent ) {
                        entry->highlight_as_parent = true;
                        break;
                    }
                }
            }
            // Find contents of selected.
            if( !entry->any_item().has_parent() ) {
                continue;
            }
            // More than one item can be highlighted when selected container is stacked.
            for( const item_location &location : selected.locations ) {
                if( entry->any_item().parent_item() == location ) {
                    entry->highlight_as_child = true;
                }
            }
        }
    }
}
inventory_multiselector::inventory_multiselector( cataimgui::window *parent, Character &p,
        const inventory_selector_preset &preset,
        const std::string &selection_column_title,
        const GetStats &get_stats,
        const bool allow_select_contained ) :
    inventory_selector( parent, p, preset ),
    allow_select_contained( allow_select_contained ),
    selection_col( new selection_column( this, "SELECTION_COLUMN", selection_column_title ) ),
    get_stats( get_stats )
{
    ctxt.register_action( "TOGGLE_ENTRY", to_translation( "Mark/unmark selected item" ) );
    ctxt.register_action( "MARK_WITH_COUNT",
                          to_translation( "Mark a specific amount of selected item" ) );
    ctxt.register_action( "TOGGLE_NON_FAVORITE", to_translation( "Mark/unmark non-favorite items" ) );
    ctxt.register_action( "INCREASE_COUNT" );
    ctxt.register_action( "DECREASE_COUNT" );

    max_chosen_count = std::numeric_limits<decltype( max_chosen_count )>::max();

    set_multiselect( true );
    append_column( *selection_col );
}

inventory_multiselector::inventory_multiselector( Character &p,
        const inventory_selector_preset &preset,
        const std::string &selection_column_title,
        const GetStats &get_stats,
        const bool allow_select_contained ) :
    inventory_selector( p, preset ),
    allow_select_contained( allow_select_contained ),
    selection_col( new selection_column( this, "SELECTION_COLUMN", selection_column_title ) ),
    get_stats( get_stats )
{
    ctxt.register_action( "TOGGLE_ENTRY", to_translation( "Mark/unmark selected item" ) );
    ctxt.register_action( "MARK_WITH_COUNT",
                          to_translation( "Mark a specific amount of selected item" ) );
    ctxt.register_action( "TOGGLE_NON_FAVORITE", to_translation( "Mark/unmark non-favorite items" ) );
    ctxt.register_action( "INCREASE_COUNT" );
    ctxt.register_action( "DECREASE_COUNT" );

    max_chosen_count = std::numeric_limits<decltype( max_chosen_count )>::max();

    set_multiselect( true );
    append_column( *selection_col );
}

bool inventory_multiselector::is_mine( inventory_entry &entry )
{
    for( inventory_column *col : columns ) {
        std::vector<inventory_entry *> entries = col->get_entries( []( const inventory_entry & en ) {
            return true;
        } );
        for( inventory_entry *ent : entries ) {
            if( &entry == ent ) {
                return true;
            }
        }
    }
    return false;
}

void inventory_multiselector::toggle_entry( inventory_entry &entry, size_t count )
{
    // moving the mouse on the trader UI causes several weird edge cases. the below line is a catch-all to
    //  prevent a weird case where toggle_entry is called with a different selector's entry.
    if( !is_mine( entry ) ) {
        return;
    }
    set_chosen_count( entry, count );
    on_toggle();
    selection_col->prepare_paging();
}

void inventory_multiselector::rearrange_columns( size_t client_width )
{
    selection_col->set_visibility( true );
    inventory_selector::rearrange_columns( client_width );
    selection_col->set_visibility( client_width >= 700 );
}

void inventory_multiselector::set_chosen_count( inventory_entry &entry, size_t count )
{
    const item_location &it = entry.any_item();

    /* Since we're modifying selection of this entry, we need to clear out
       anything that's been set before.
     */
    for( const item_location &loc : entry.locations ) {
        for( auto iter = to_use.begin(); iter != to_use.end(); ) {
            if( iter->first == loc ) {
                iter = to_use.erase( iter );
            } else {
                ++iter;
            }
        }
    }

    if( count == 0 ) {
        entry.chosen_count = 0;
    } else {
        entry.chosen_count = std::min( {count, max_chosen_count, entry.get_available_count() } );
        if( it->count_by_charges() ) {
            auto iter = find_if( to_use.begin(), to_use.end(), [&it]( const drop_location & drop ) {
                return drop.first == it;
            } );
            if( iter == to_use.end() ) {
                to_use.emplace_back( it, static_cast<int>( entry.chosen_count ) );
            }
        } else {
            for( const item_location &loc : entry.locations ) {
                if( count == 0 ) {
                    break;
                }
                auto iter = find_if( to_use.begin(), to_use.end(), [&loc]( const drop_location & drop ) {
                    return drop.first == loc;
                } );
                if( iter == to_use.end() ) {
                    to_use.emplace_back( loc, 1 );
                }
                count--;
            }
        }
    }

    on_change( entry );
}

void inventory_multiselector::toggle_entries( int &count, const toggle_mode mode, bool mouse_only )
{
    inventory_entry *selected = nullptr;
    switch( mode ) {
        case toggle_mode::SELECTED:
            if( mouse_only ) {
                selected = mouse_hovered_entry;
            } else {
                selected = &get_highlighted();
            }
            break;
        case toggle_mode::NON_FAVORITE_NON_WORN: {
            const auto filter_to_nonfavorite_and_nonworn = [this]( const inventory_entry & entry ) {
                return entry.is_selectable() &&
                       !entry.any_item()->is_favorite &&
                       !u.is_worn( *entry.any_item() );
            };
            auto entries_tmp = columns[active_column_index]->get_entries( filter_to_nonfavorite_and_nonworn );
            if( !entries_tmp.empty() ) {
                selected = entries_tmp[0];
            }
        }
    }

    if( selected == nullptr || !selected->is_selectable() || !is_mine( *selected ) ) {
        count = 0;
        return;
    }

    // No amount entered, select all
    if( count == 0 ) {
        bool select_nonfav = true;
        bool select_fav = true;
        switch( mode ) {
            case toggle_mode::SELECTED: {
                count = INT_MAX;

                // Any non favorite item to select?
                select_nonfav = !selected->any_item()->is_favorite && selected->chosen_count == 0;

                // Otherwise, any favorite item to select?
                select_fav = !select_nonfav && selected->any_item()->is_favorite && selected->chosen_count == 0;
                break;
            }
            case toggle_mode::NON_FAVORITE_NON_WORN: {
                const bool clear = selected->chosen_count == 0;

                if( clear ) {
                    count = max_chosen_count;
                }
                break;
            }
        }

        const bool is_favorite = selected->any_item()->is_favorite;
        if( ( select_nonfav && !is_favorite ) || ( select_fav && is_favorite ) ) {
            set_chosen_count( *selected, count );
        } else if( !select_nonfav && !select_fav ) {
            // Every element is selected, unselect all
            set_chosen_count( *selected, 0 );
        }
        // Select the entered amount
    } else {
        set_chosen_count( *selected, count );
    }

    if( !allow_select_contained ) {
        deselect_contained_items();
    }

    selection_col->prepare_paging();
    count = 0;
    on_toggle();
}

drop_locations inventory_multiselector::execute()
{
    for( inventory_column *col : columns ) {
        col->prepare_paging();
    }
    debug_print_timer( tp_start );
    while( true ) {
        ui_manager::redraw();

        const inventory_input input = get_input();

        if( input.action == "CONFIRM" ) {
            if( to_use.empty() ) {
                popup_getkey( _( "No items were selected.  Use %s to select them." ),
                              ctxt.get_desc( "TOGGLE_ENTRY" ) );
                continue;
            }
            break;
        }

        if( input.action == "QUIT" ) {
            return drop_locations();
        }

        on_input( input );
    }
    drop_locations dropped_pos_and_qty;
    for( const std::pair<item_location, int> &drop_pair : to_use ) {
        dropped_pos_and_qty.push_back( drop_pair );
    }

    return dropped_pos_and_qty;
}

inventory_compare_selector::inventory_compare_selector( Character &p ) :
    inventory_multiselector( p, default_preset, _( "ITEMS TO COMPARE" ) ) {}

std::pair<const item *, const item *> inventory_compare_selector::execute()
{
    for( inventory_column *col : columns ) {
        col->prepare_paging();
    }
    debug_print_timer( tp_start );
    while( true ) {
        ui_manager::redraw();

        const inventory_input input = get_input();

        inventory_entry *just_selected = nullptr;

        if( input.entry != nullptr ) {
            highlight( input.entry->any_item() );
            if( input.action == "SELECT" || input.action == "ANY_INPUT" ) {
                toggle_entry( input.entry );
                just_selected = input.entry;
            } else if( input.action != "MOUSE_MOVE" ) {
                inventory_selector::on_input( input );
            }
        } else if( input.action == "TOGGLE_ENTRY" ) {
            auto selection( get_all_highlighted() );

            for( inventory_entry *elem : selection ) {
                if( elem->chosen_count == 0 || selection.size() == 1 ) {
                    toggle_entry( elem );
                    just_selected = elem;
                    if( compared.size() == 2 ) {
                        break;
                    }
                }
            }
        } else if( input.action == "CONFIRM" ) {
            popup_getkey( _( "You need two items for comparison.  Use %s to select them." ),
                          ctxt.get_desc( "TOGGLE_ENTRY" ) );
        } else if( input.action == "QUIT" ) {
            return std::make_pair( nullptr, nullptr );
        } else if( input.action == "TOGGLE_FAVORITE" ) {
            // TODO: implement favoriting in multi selection menus while maintaining selection
        } else {
            inventory_selector::on_input( input );
        }

        if( compared.size() == 2 ) {
            const auto res = std::make_pair( compared[0], compared[1] );
            // Clear second selected entry to prevent comparison reopening too
            // soon
            if( just_selected ) {
                toggle_entry( just_selected );
            }
            return res;
        }
    }
}

void inventory_compare_selector::toggle_entry( inventory_entry *entry )
{
    const item *it = &*entry->any_item();
    const auto iter = std::find( compared.begin(), compared.end(), it );

    entry->chosen_count = iter == compared.end() ? 1 : 0;

    if( entry->chosen_count != 0 ) {
        compared.push_back( it );
    } else {
        compared.erase( iter );
    }

    on_change( *entry );
}

inventory_selector::stats inventory_multiselector::get_raw_stats() const
{
    if( get_stats ) {
        return get_stats( to_use );
    }
    return stats{{ stat{{ "", "", "", "" }}, stat{{ "", "", "", "" }} }};
}

inventory_drop_selector::inventory_drop_selector( cataimgui::window *parent, Character &p,
        const inventory_selector_preset &preset,
        const std::string &selection_column_title,
        const bool warn_liquid ) :
    inventory_multiselector( parent, p, preset, selection_column_title ),
    warn_liquid( warn_liquid )
{
#if defined(__ANDROID__)
    // allow user to type a drop number without dismissing virtual keyboard after each keypress
    ctxt.allow_text_entry = true;
#endif
}

inventory_drop_selector::inventory_drop_selector( Character &p,
        const inventory_selector_preset &preset,
        const std::string &selection_column_title,
        const bool warn_liquid ) :
    inventory_multiselector( p, preset, selection_column_title ),
    warn_liquid( warn_liquid )
{
#if defined(__ANDROID__)
    // allow user to type a drop number without dismissing virtual keyboard after each keypress
    ctxt.allow_text_entry = true;
#endif
}

inventory_insert_selector::inventory_insert_selector( Character &p,
        const inventory_holster_preset &preset,
        const std::string &selection_column_title,
        const bool warn_liquid ) :
    inventory_drop_selector( p, preset, selection_column_title, warn_liquid )
{
#if defined(__ANDROID__)
    // allow user to type a drop number without dismissing virtual keyboard after each keypress
    ctxt.allow_text_entry = true;
#endif
}

void inventory_multiselector::deselect_contained_items()
{
    std::vector<item_location> inventory_items;
    for( std::pair<item_location, int> &drop : to_use ) {
        item_location loc_front = drop.first;
        inventory_items.push_back( loc_front );
    }
    for( item_location loc_container : inventory_items ) {
        if( !loc_container->empty() ) {
            for( inventory_column *col : get_all_columns() ) {
                for( inventory_entry *selected : col->get_entries( []( const inventory_entry & entry ) {
                return entry.chosen_count > 0;
            } ) ) {
                    if( !selected->is_item() ) {
                        continue;
                    }
                    for( const item *item_contained : loc_container->all_items_ptr() ) {
                        for( const item_location &selected_loc : selected->locations ) {
                            if( selected_loc.get_item() == item_contained ) {
                                set_chosen_count( *selected, 0 );
                            }
                        }
                    }
                }
            }
        }
    }
    for( inventory_column *col : get_all_columns() ) {
        for( inventory_entry *selected : col->get_entries(
        []( const inventory_entry & entry ) {
        return entry.is_item() && entry.chosen_count > 0 && entry.locations.front()->is_frozen_liquid() &&
                   //Frozen liquids can be selected if it have the SHREDDED flag.
                   !entry.locations.front()->has_flag( STATIC( flag_id( "SHREDDED" ) ) ) &&
                   (
                       ( //Frozen liquids on the map are not selectable if they can't be crushed.
                           entry.locations.front().where() == item_location::type::map &&
                           !get_player_character().can_crush_frozen_liquid( entry.locations.front() ).success() ) ||
                       ( //Weapon in hand is can selectable.
                           entry.locations.front().where() == item_location::type::character &&
                           !entry.locations.front().has_parent() &&
                           entry.locations.front() != get_player_character().used_weapon() ) ||
                       ( //Frozen liquids are unselectable if they don't have SHREDDED flag and can't be crushed in a container.
                           entry.locations.front().has_parent() &&
                           entry.locations.front().where() == item_location::type::container &&
                           !get_player_character().can_crush_frozen_liquid( entry.locations.front() ).success() )
                   );
        } ) ) {
            set_chosen_count( *selected, 0 );
        }
    }
}

void inventory_multiselector::toggle_categorize_contained()
{
    selection_col->clear();
    inventory_selector::toggle_categorize_contained();

    for( inventory_column *col : get_all_columns() ) {
        for( inventory_entry *entry : col->get_entries( return_item, true ) ) {
            if( entry->chosen_count > 0 ) {
                toggle_entry( *entry, entry->chosen_count );
            }
        }
    }
}

void inventory_multiselector::on_input( const inventory_input &input )
{
    if( input.action == "SELECT" ) {
        toggle_entries( count, toggle_mode::SELECTED, true );
    } else if( input.action == "TOGGLE_NON_FAVORITE" ) {
        toggle_entries( count, toggle_mode::NON_FAVORITE_NON_WORN );
    } else if( input.action == "MARK_WITH_COUNT" ) { // Set count and mark selected with specific key
        int query_result = query_count();
        if( query_result >= 0 ) {
            toggle_entries( query_result, toggle_mode::SELECTED );
        }
    } else if( !uistate.numpad_navigation && input.ch >= '0' && input.ch <= '9' ) {
        int query_result = query_count( input.ch, true );
        if( query_result >= 0 ) {
            toggle_entries( query_result, toggle_mode::SELECTED );
        }
    } else if( input.action == "TOGGLE_ENTRY" ) { // Mark selected
        toggle_entries( count, toggle_mode::SELECTED );
    } else if( input.action == "INCREASE_COUNT" || input.action == "DECREASE_COUNT" ) {
        inventory_entry &entry = get_highlighted();
        if( entry.is_selectable() ) {
            size_t const count = entry.chosen_count;
            size_t const max = entry.get_available_count();
            size_t const newcount = input.action == "INCREASE_COUNT" ?
                                    count < max ? count + 1 : max
                                    : count > 1
                                    ? count - 1 : 0;
            toggle_entry( entry, newcount );
        }
    } else if( input.action == "VIEW_CATEGORY_MODE" ) {
        toggle_categorize_contained();
    } else {
        inventory_selector::on_input( input );
    }
}

drop_locations inventory_drop_selector::execute()
{
    for( inventory_column *col : columns ) {
        col->prepare_paging();
    }
    debug_print_timer( tp_start );
    while( true ) {
        ui_manager::redraw();

        const inventory_input input = get_input();
        if( input.action == "CONFIRM" ) {
            if( to_use.empty() ) {
                popup_getkey( _( "No items were selected.  Use %s to select them." ),
                              ctxt.get_desc( "TOGGLE_ENTRY" ) );
                continue;
            }
            break;
        }

        if( input.action == "QUIT" ) {
            return drop_locations();
        }

        on_input( input );
    }

    drop_locations dropped_pos_and_qty;

    enum class drop_liquid {
        ask, no, yes
    } should_drop_liquid = drop_liquid::ask;

    for( const std::pair<item_location, int> &drop_pair : to_use ) {
        bool should_drop = true;
        if( drop_pair.first->made_of_from_type( phase_id::LIQUID ) &&
            !drop_pair.first->is_frozen_liquid() ) {
            if( should_drop_liquid == drop_liquid::ask ) {
                if( !warn_liquid ||
                    show_popup( new cataimgui::message_box( _( "Warning" ),
                                _( "You are dropping liquid from its container.  You might not be able to pick it back up.  Really do so?" ),
                                cataimgui::mbox_btn::YesNo ) ) == cataimgui::dialog_result::YesClicked ) {
                    should_drop_liquid = drop_liquid::yes;
                } else {
                    should_drop_liquid = drop_liquid::no;
                }
            }
            if( should_drop_liquid == drop_liquid::no ) {
                should_drop = false;
            }
        }
        if( should_drop ) {
            dropped_pos_and_qty.push_back( drop_pair );
        }
    }

    return dropped_pos_and_qty;
}

inventory_selector::stats inventory_drop_selector::get_raw_stats() const
{
    return get_weight_and_volume_and_holster_stats(
               u.weight_carried_with_tweaks( to_use ),
               u.weight_capacity(),
               u.volume_carried_with_tweaks( to_use ),
               u.volume_capacity_with_tweaks( to_use ),
               u.max_single_item_length(), u.max_single_item_volume(),
               u.free_holster_volume(), u.used_holsters(), u.total_holsters() );
}

inventory_selector::stats inventory_insert_selector::get_raw_stats() const
{
    units::mass selected_weight = units::mass();
    units::volume selected_volume = units::volume();
    const item_location &holster = static_cast<const inventory_holster_preset &>
                                   ( preset ).get_holster();
    const std::vector<inventory_column *> &columns = get_all_columns();
    int holstered_items = 0;
    units::volume holster_volume = units::volume();
    units::mass holster_weight = units::mass();
    std::vector<const item_pocket *> used_pockets = std::vector<const item_pocket *>();
    for( const inventory_column *c : columns ) {
        if( c == nullptr ) {
            continue;
        }
        if( c->allows_selecting() ) {
            const inventory_column::get_entries_t entries = c->get_entries( always_yes );
            for( const inventory_entry *e : entries ) {
                if( e == nullptr ) {
                    continue;
                }
                if( e->chosen_count == 0 ) {
                    continue;
                }
                const item *item_to_insert = e->any_item().get_item();
                if( item_to_insert == nullptr ) {
                    continue;
                }
                units::mass w = item_to_insert->weight();
                units::volume v = item_to_insert->volume();
                int overflow_counter = e->chosen_count;
                for( const item_pocket *p : holster.get_item()->get_all_contained_pockets() ) {
                    bool pocket_already_used = false;
                    for( const item_pocket *used_pocket : used_pockets ) {
                        if( p == used_pocket ) {
                            pocket_already_used = true;
                            break;
                        }
                    }
                    if( !pocket_already_used && p->is_holster() && !p->holster_full() ) {
                        ret_val<item_pocket::contain_code> contain = p->can_contain( *item_to_insert );
                        if( contain.success() ) {
                            bool has_better_pocket = false;
                            for( const item_pocket *other_pocket : holster.get_item()->get_all_contained_pockets() ) {
                                if( p == other_pocket ) {
                                    continue;
                                }
                                if( p->better_pocket( *other_pocket, *item_to_insert, false ) ) {
                                    has_better_pocket = true;
                                }
                            }
                            if( has_better_pocket ) {
                                continue;
                            }
                            holstered_items += 1;
                            holster_weight += w;
                            holster_volume += v;
                            used_pockets.push_back( p );
                            overflow_counter -= 1;
                            if( overflow_counter <= 0 ) {
                                break;
                            }
                        }
                    }
                }
                selected_weight += w * overflow_counter;
                selected_volume += v * overflow_counter;
            }
        }
    }

    units::mass contained_weight = holster->get_total_contained_weight() + selected_weight +
                                   holster_weight +
                                   holster->get_used_holster_weight();
    units::mass total_weight = holster->get_total_weight_capacity() + holster_weight;
    units::volume contained_volume = holster->get_total_contained_volume() + selected_volume +
                                     holster_volume;
    units::volume total_volume = holster->get_total_capacity();
    return get_weight_and_volume_and_holster_stats( contained_weight,
            total_weight,
            contained_volume,
            total_volume,
            holster->max_containable_length(),
            holster->max_containable_volume(),
            holster->get_total_holster_volume() - ( holster->get_used_holster_volume() + holster_volume ),
            holster->get_used_holsters() + holstered_items,
            holster->get_total_holsters() );
}



pickup_selector::pickup_selector( Character &p, const inventory_selector_preset &preset,
                                  const std::string &selection_column_title, const std::optional<tripoint> &where ) :
    inventory_multiselector( p, preset, selection_column_title ), where( where )
{
    ctxt.register_action( "WEAR" );
    ctxt.register_action( "WIELD" );
#if defined(__ANDROID__)
    // allow user to type a drop number without dismissing virtual keyboard after each keypress
    ctxt.allow_text_entry = true;
#endif

    set_hint( string_format(
                  _( "%s wield %s wear\n%s expand %s all\n%s examine %s/%s/%s quantity (or type number then %s)" ),
                  colorize( ctxt.get_desc( "WIELD" ), c_yellow ),
                  colorize( ctxt.get_desc( "WEAR" ), c_yellow ),
                  colorize( ctxt.get_desc( "SHOW_HIDE_CONTENTS" ), c_yellow ),
                  colorize( ctxt.get_desc( "SHOW_HIDE_CONTENTS_ALL" ), c_yellow ),
                  colorize( ctxt.get_desc( "EXAMINE" ), c_yellow ),
                  colorize( ctxt.get_desc( "MARK_WITH_COUNT" ), c_yellow ),
                  colorize( ctxt.get_desc( "INCREASE_COUNT" ), c_yellow ),
                  colorize( ctxt.get_desc( "DECREASE_COUNT" ), c_yellow ),
                  colorize( ctxt.get_desc( "TOGGLE_ENTRY" ), c_yellow ) ) );
}

void pickup_selector::apply_selection( std::vector<drop_location> selection )
{
    for( drop_location &loc : selection ) {
        inventory_entry *entry = find_entry_by_location( loc.first );
        if( entry != nullptr ) {
            set_chosen_count( *entry, loc.second + entry->chosen_count );
        }
    }
}

drop_locations pickup_selector::execute()
{
    for( inventory_column *col : columns ) {
        col->prepare_paging();
    }
    debug_print_timer( tp_start );

    while( true ) {
        ui_manager::redraw();

        const inventory_input input = get_input();

        if( input.action == "CONFIRM" ) {
            if( to_use.empty() ) {
                popup_getkey( _( "No items were selected.  Use %s to select them." ),
                              ctxt.get_desc( "TOGGLE_ENTRY" ) );
                continue;
            }
            break;
        } else if( input.action == "WIELD" ) {
            if( wield( count ) ) {
                return drop_locations();
            }
        } else if( input.action == "WEAR" ) {
            if( wear() ) {
                return drop_locations();
            }
        } else if( input.action == "QUIT" ) {
            return drop_locations();
        } else {
            on_input( input );
        }
    }
    drop_locations dropped_pos_and_qty;
    for( const std::pair<item_location, int> &drop_pair : to_use ) {
        dropped_pos_and_qty.push_back( drop_pair );
    }

    return dropped_pos_and_qty;
}

bool pickup_selector::wield( int &count )
{
    inventory_entry &selected = get_highlighted();
    if( !selected.is_item() ) {
        return false;
    }

    item_location it = selected.any_item();
    if( count == 0 ) {
        count = INT_MAX;
    }
    int charges = std::min( it->charges, count );

    if( u.can_wield( *it ).success() ) {
        remove_from_to_use( it );
        add_reopen_activity();
        u.assign_activity( wield_activity_actor( it, charges ) );
        return true;
    } else {
        popup_getkey( _( "You can't wield the %s." ), it->display_name() );
    }

    return false;
}

bool pickup_selector::wear()
{
    inventory_entry &selected = get_highlighted();
    if( !selected.is_item() ) {
        return false;
    }

    std::vector<item_location> items{ selected.any_item() };
    std::vector<int> quantities{ 0 };

    if( u.can_wear( *items.front() ).success() ) {
        remove_from_to_use( items.front() );
        add_reopen_activity();
        u.assign_activity( wear_activity_actor( items, quantities ) );
        return true;
    } else {
        popup_getkey( _( "You can't wear the %s." ), items.front()->display_name() );
    }

    return false;
}

void pickup_selector::add_reopen_activity()
{
    u.assign_activity( pickup_menu_activity_actor( where, to_use ) );
    u.activity.auto_resume = true;
}

void pickup_selector::remove_from_to_use( item_location &it )
{
    for( auto iter = to_use.begin(); iter < to_use.end(); ) {
        if( iter->first == it ) {
            to_use.erase( iter );
            return;
        } else {
            iter++;
        }
    }
}

void pickup_selector::reassign_custom_invlets()
{
    if( invlet_type() == SELECTOR_INVLET_DEFAULT ) {
        set_invlet_type( SELECTOR_INVLET_ALPHA );
    }
    inventory_selector::reassign_custom_invlets();
}

inventory_selector::stats pickup_selector::get_raw_stats() const
{
    units::mass weight;
    units::volume volume;

    for( const drop_location &loc : to_use ) {
        if( loc.first->count_by_charges() ) {
            item copy( *loc.first.get_item() );
            copy.charges = loc.second;
            weight += copy.weight();
            volume += copy.volume();
        } else {
            weight += loc.first->weight() * loc.second;
            volume += loc.first->volume() * loc.second;
        }
    }

    return get_weight_and_volume_and_holster_stats(
               u.weight_carried() + weight,
               u.weight_capacity(),
               u.volume_carried() + volume,
               u.volume_capacity(),
               u.max_single_item_length(),
               u.max_single_item_volume(),
               u.free_holster_volume(),
               u.used_holsters(),
               u.total_holsters() );
}

inventory_examiner::inventory_examiner( Character &p,
                                        item_location item_to_look_inside,
                                        const inventory_selector_preset &preset ) :
    inventory_selector( p, preset )
{
    force_max_window_size();
    selected_item = item_location::nowhere;
    parent_item = std::move( item_to_look_inside );
    changes_made = false;
    parent_was_collapsed = false;

    //Space in inventory isn't particularly relevant, so don't display it
    set_display_stats( false );

    setup();
}

bool inventory_examiner::check_parent_item()
{
    return !( parent_item->is_container_empty() || empty() );
}

int inventory_examiner::cleanup() const
{
    if( changes_made ) {
        return EXAMINED_CONTENTS_WITH_CHANGES;
    } else {
        return EXAMINED_CONTENTS_UNCHANGED;
    }
}

void inventory_examiner::draw_item_details( const item_location &sitem )
{
    std::vector<iteminfo> vThisItem;
    std::vector<iteminfo> vDummy;

    sitem->info( true, vThisItem );

    item_info_data data( sitem->tname(), sitem->type_name(), vThisItem, vDummy );
    data.without_getch = true;

    draw_item_info_data( data );
}

void inventory_examiner::force_max_window_size()
{
    constexpr int border_width = 1;
    _fixed_bounds.emplace( cataimgui::bounds{0.0f, 0.0f, float( get_window_width() / 3.0f + 2 * border_width ), float( get_window_height() )} );
}

int inventory_examiner::execute()
{
    if( !check_parent_item() ) {
        return NO_CONTENTS_TO_EXAMINE;
    }
    for( inventory_column *col : columns ) {
        col->prepare_paging();
    }

    // pass 0 into this to ensure we get 1 column
    rearrange_columns( 0 );
    debug_print_timer( tp_start );


    while( true ) {
#if !(defined(WIN32) || defined(TILES))
        // only needed for ImTui - NCurses mode
        ui_manager::redraw();
#endif

        const inventory_input input = get_input();

        if( input.entry != nullptr ) {
            if( input.action == "SELECT" ) {
                return cleanup();
            }
        }

        if( input.action == "QUIT" || input.action == "CONFIRM" ) {
            return cleanup();
        } else {
            if( input.action == "SHOW_HIDE_CONTENTS" ) {
                changes_made = true;
            }
            on_input( input );
        }
    }
}

cataimgui::bounds inventory_examiner::get_bounds()
{
    return{ 0.0F, 0.0F, float( get_window_width() ), float( get_window_height() ) };
}

void inventory_examiner::draw_controls()
{
    inventory_entry *active_entry = nullptr;
    if( ImGui::BeginTable( "EXAMINER_TABLE", 2, ImGuiTableFlags_Borders ) ) {
        ImGui::TableSetupColumn( "ITEM_LIST", ImGuiTableColumnFlags_WidthFixed,
                                 get_window_width() * 0.25F );
        ImGui::TableSetupColumn( "ITEM_INFO", ImGuiTableColumnFlags_WidthFixed,
                                 get_window_width() * 0.75F );
        ImGui::TableNextColumn();
        // draw a child window here - this allows scrolling in this area that doesn't affect the rest of the window
        if( ImGui::BeginChild( "ITEM_LIST_CHILDWIN" ) )             {
            for( inventory_column *col : columns ) {
                if( col->visible() ) {
                    // draw only the first visible column
                    draw_column( col );
                    break;
                }
            }
            if( mouse_hovered_entry != nullptr ) {
                active_entry = mouse_hovered_entry;
            } else if( keyboard_focused_entry != nullptr ) {
                active_entry = keyboard_focused_entry;
            }
        }
        ImGui::EndChild();
        ImGui::TableNextColumn();
        if( ImGui::BeginChild( "ITEM_INFO_CHILDWIN" ) ) {
            if( active_entry ) {
                if( selected_item != active_entry->any_item() ) {
                    selected_item = active_entry->any_item();
                }
                draw_item_details( selected_item );
            } else {
                ImGui::Text( "No focused entry." );
            }
        }
        ImGui::EndChild();
        ImGui::EndTable();
    }
}

void inventory_examiner::setup()
{
    if( parent_item == item_location::nowhere ) {
        set_title( "ERROR: Item not found" );
    } else {
        set_title( parent_item->display_name() );
    }
}

void inventory_selector::categorize_map_items( bool toggle )
{
    _categorize_map_items = toggle;
}

cataimgui::bounds inventory_selector::get_bounds()
{
    if( _fixed_bounds.has_value() ) {
        return _fixed_bounds.value();
    } else {
        return { get_window_width() * 0.2f, 0, get_window_width() * 0.6f, float( get_window_height() ) };
    }
}

void inventory_selector::draw_controls()
{
    int old_x_pos = ImGui::GetCursorPosX();
    ImGui::Text( "%s", remove_color_tags( hint ).c_str() );
    ImGui::SameLine();
    if( display_stats ) {
        nc_color tcolor = c_light_gray;
        for( const std::string &elem : get_stats() ) {
            draw_colored_text( elem, tcolor, cataimgui::text_align::Right );
        }
    }
    if( show_buttons ) {
        ImGui::SameLine();
        ImGui::SetCursorPosX( old_x_pos );
        action_button( "CONFIRM", "Confirm" );
        ImGui::SameLine();
        action_button( "INVENTORY_FILTER", "Filter" );
        ImGui::SameLine();
        action_button( "VIEW_CATEGORY_MODE", "Change View Mode" );
    }
    if( is_resized() ) {
        prepare_layout();
    }
    int num_columns = 0;
    for( auto col : columns ) {
        if( col->visible() ) {
            num_columns++;
        }
    }
    ImGui::Separator();
    float table_height = ImGui::GetContentRegionAvail().y;
    auto footer = get_footer( this->mode );
    if( !footer.first.empty() ) {
        table_height -= ImGui::GetTextLineHeightWithSpacing();
    };
    if( num_columns == 0 ) {
        ImGui::SetCursorPosY( ImGui::GetCursorPosY() + table_height );
    } else {
        if( ImGui::BeginTable( "inventory columns", num_columns,
                               ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_Borders, { 0.0f, table_height } ) ) {
            ImGui::TableNextColumn();
            int drawn_columns = 0;
            for( auto column : columns ) {
                if( column->visible() ) {
                    if( ImGui::BeginChild( string_format( "COLUMN_%d", drawn_columns ).c_str() ) ) {
                        inventory_entry &selected_entry = draw_column( column );
                    }
                    ImGui::EndChild();
                    if( ++drawn_columns < num_columns ) {
                        ImGui::TableNextColumn();
                    }
                }
            }
            ImGui::EndTable();
        }
    }
    if( !footer.first.empty() ) {
        draw_colored_text( footer.first, footer.second );
    }
}
