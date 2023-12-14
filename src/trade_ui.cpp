
#include "trade_ui.h"

#include <cmath>
#include <cstdlib>
#include <memory>

#include "character.h"
#include "clzones.h"
#include "color.h"
#include "enums.h"
#include "game_constants.h"
#include "inventory_ui.h"
#include "item.h"
#include "npc.h"
#include "npctrade.h"
#include "npctrade_utils.h"
#include "output.h"
#include "point.h"
#include "string_formatter.h"
#include "type_id.h"
#include "messages.h"
#include "imgui/imgui.h"
#include "cata_imgui.h"

static const flag_id json_flag_NO_UNWIELD( "NO_UNWIELD" );
static const item_category_id item_category_ITEMS_WORN( "ITEMS_WORN" );
static const item_category_id item_category_WEAPON_HELD( "WEAPON_HELD" );

namespace
{
cataimgui::bounds _get_pane_bounds( int side )
{
    return { side > 0 ? ImGui::GetContentRegionAvail().x / 2.0f + ImGui::GetCursorPosX() : -1.0f, ImGui::GetCursorPosY(), ImGui::GetContentRegionAvail().x / 2.0f, float( ImGui::GetContentRegionAvail().y )};
}

} // namespace

trade_preset::trade_preset( Character const &you, Character const &trader )
    : _u( you ), _trader( trader )
{
    save_state = &inventory_ui_default_state;
    append_cell(
    [&]( item_location const & loc ) {
        return format_money( npc_trading::trading_price( _trader, _u, { loc, 1 } ) );
    },
    _( "Unit price" ) );
}

bool trade_preset::is_shown( item_location const &loc ) const
{
    return !loc->has_var( VAR_TRADE_IGNORE ) && inventory_selector_preset::is_shown( loc ) &&
           loc->is_owned_by( _u ) && loc->made_of( phase_id::SOLID ) && !loc->is_frozen_liquid() &&
           ( !_u.is_wielding( *loc ) || !loc->has_flag( json_flag_NO_UNWIELD ) );
}

std::string trade_preset::get_denial( const item_location &loc ) const
{
    int const price = npc_trading::trading_price( _trader, _u, { loc, 1 } );

    if( _u.is_npc() ) {
        npc const &np = *_u.as_npc();
        ret_val<void> const ret = np.wants_to_sell( loc, price );
        if( !ret.success() ) {
            if( ret.str().empty() ) {
                return string_format( _( "%s does not want to sell this" ), np.get_name() );
            }
            return np.replace_with_npc_name( ret.str() );
        }
    } else if( _trader.is_npc() ) {
        npc const &np = *_trader.as_npc();
        ret_val<void> const ret = np.wants_to_buy( *loc, price );
        if( !ret.success() ) {
            if( ret.str().empty() ) {
                return string_format( _( "%s does not want to buy this" ), np.get_name() );
            }
            return np.replace_with_npc_name( ret.str() );
        }
    }

    if( _u.is_worn( *loc ) ) {
        ret_val<void> const ret = const_cast<Character &>( _u ).can_takeoff( *loc );
        if( !ret.success() ) {
            return _u.replace_with_npc_name( ret.str() );
        }
    }

    return inventory_selector_preset::get_denial( loc );
}

bool trade_preset::cat_sort_compare( const inventory_entry &lhs, const inventory_entry &rhs ) const
{
    // sort worn and held categories last we likely don't want to trade them
    auto const fudge_rank = []( inventory_entry const & e ) -> int {
        item_category_id const cat = e.get_category_ptr()->get_id();
        int const rank = e.get_category_ptr()->sort_rank();
        return cat != item_category_ITEMS_WORN && cat != item_category_WEAPON_HELD ? rank : rank + 10000;
    };
    return fudge_rank( lhs ) < fudge_rank( rhs );
}

trade_ui::trade_ui( party_t &you, npc &trader, currency_t cost, std::string title )
    : cataimgui::window( title, ImGuiWindowFlags_AlwaysAutoResize ),
      _upreset{ you, trader }, _tpreset{ trader, you },
      _panes { std::make_unique<pane_t>( this, trader, _tpreset, std::string() ),
               std::make_unique<pane_t>( this, you, _upreset, std::string() ) },
      _parties{ &trader, &you }, _title( std::move( title ) )

{
    window_size.x = 0;
    window_size.y = 0;
    _panes[_you]->add_character_items( you );
    _panes[_you]->add_nearby_items( 1 );
    _panes[_trader]->add_character_items( trader );
    _panes[_trader]->set_title( "Trader Inventory" ); // DO NOT REMOVE: ImGui needs each child window to have unique titles/IDs
    _panes[_you]->set_title( "Your Inventory" );
    if( trader.is_shopkeeper() ) {
        _panes[_trader]->categorize_map_items( true );

        add_fallback_zone( trader );

        zone_manager &zmgr = zone_manager::get_manager();

        // FIXME: migration for traders in old saves - remove after 0.G
        zone_data const *const fallback =
            zmgr.get_zone_at( trader.get_location(), true, trader.get_fac_id() );
        bool const legacy = fallback != nullptr && fallback->get_name() == fallback_name;

        if( legacy ) {
            _panes[_trader]->add_nearby_items( PICKUP_RANGE );
        } else {
            std::unordered_set<tripoint> const src =
                zmgr.get_point_set_loot( trader.get_location(), PICKUP_RANGE, trader.get_fac_id() );

            for( tripoint const &pt : src ) {
                _panes[_trader]->add_map_items( pt );
                _panes[_trader]->add_vehicle_items( pt );
            }
        }
    } else if( !trader.is_player_ally() ) {
        _panes[_trader]->add_nearby_items( 1 );
    }

    if( trader.will_exchange_items_freely() ) {
        _cost = 0;
    } else {
        _cost = trader.op_of_u.owed - cost;
    }
    _balance = _cost;

    _panes[_you]->on_deactivate();
}

void trade_ui::pushevent( event const &ev )
{
    _queue.emplace( ev );
}

trade_ui::trade_result_t trade_ui::perform_trade()
{
    _exit = false;
    _traded = false;
    while( !_exit ) {
        bool no_auto_cpane_switch = false;
        _panes[_cpane]->execute();

        while( !_queue.empty() ) {
            no_auto_cpane_switch = true;
            event const ev = _queue.front();
            _queue.pop();
            _process( ev );
        }
        if( !no_auto_cpane_switch ) {
            for( int new_cpane = 0; new_cpane < 2;
                 new_cpane++ ) { // selected entry belongs to this pane? break - we've figured out the selected pane
                if( _panes[new_cpane]->mouse_hovered_entry != nullptr &&
                    _panes[new_cpane]->mouse_hovered_entry->is_item() ) {
                    _cpane = new_cpane;
                    break;
                }
            }
        }
    }

    if( _traded ) {
        return { _traded,
                 _balance,
                 _trade_values[_you],
                 _trade_values[_trader],
                 _panes[_you]->to_trade(),
                 _panes[_trader]->to_trade() };
    }

    return { false, 0, 0, 0, {}, {} };
}

void trade_ui::recalc_values_cpane()
{
    _trade_values[_cpane] = 0;

    select_t all_selected = _panes[_cpane]->to_trade();
    for( entry_t const &it : all_selected ) {
        // FIXME: cache trading_price
        _trade_values[_cpane] +=
            npc_trading::trading_price( *_parties[-_cpane + 1], *_parties[_cpane], it );
    }
    if( !_parties[_trader]->as_npc()->will_exchange_items_freely() ) {
        _balance = _cost + _trade_values[_you] - _trade_values[_trader];
    }
    //_header_ui.invalidate_ui();
}

void trade_ui::autobalance()
{
    int const sign = _cpane == _you ? -1 : 1;
    if( ( sign < 0 && _balance < 0 ) || ( sign > 0 && _balance > 0 ) ) {
        inventory_entry &entry = _panes[_cpane]->get_highlighted();
        size_t const avail = entry.get_available_count() - entry.chosen_count;
        double const price = npc_trading::trading_price( *_parties[-_cpane + 1], *_parties[_cpane],
                             entry_t{ entry.any_item(), 1 } ) * sign;
        double const num = _balance / price;
        double const extra = sign < 0 ? std::ceil( num ) : std::floor( num );
        _panes[_cpane]->toggle_entry( entry, entry.chosen_count +
                                      std::min( static_cast<size_t>( extra ), avail ) );
    }
}

void trade_ui::resize()
{
    _panes[_you]->resize( _get_pane_bounds( 1 ) );
    _panes[_trader]->resize( _get_pane_bounds( -1 ) );
}

void trade_ui::_process( event const &ev )
{
    switch( ev ) {
        case event::TRADECANCEL: {
            _traded = false;
            _exit = true;
            break;
        }
        case event::TRADEOK: {
            _traded = _confirm_trade();
            _exit = _traded;
            break;
        }
        case event::SWITCH: {
            //_panes[_cpane]->get_ui()->invalidate_ui();
            _cpane = -_cpane + 1;
            std::vector<inventory_entry *> items = _panes[_cpane]->get_items();
            if( !items.empty() ) {
                inventory_selector::entry_to_be_focused = &items[0]->any_item();
            }
            break;
        }
        case event::NEVENTS: {
            break;
        }
    }
}


bool trade_ui::_confirm_trade() const
{
    npc const &np = *_parties[_trader]->as_npc();

    if( !npc_trading::npc_will_accept_trade( np, _balance ) ) {
        if( np.max_credit_extended() == 0 ) {
            popup( _( "You'll need to offer me more than that." ) );
        } else {
            popup( _( "Sorry, I'm only willing to extend you %s in credit." ),
                   format_money( np.max_credit_extended() ) );
        }
    } else if( !np.is_shopkeeper() &&
               !npc_trading::npc_can_fit_items( np, _panes[_you]->to_trade() ) ) {
        popup( _( "%s doesn't have the appropriate pockets to accept that." ), np.get_name() );
    } else if( npc_trading::calc_npc_owes_you( np, _balance ) < _balance ) {
        // NPC is happy with the trade, but isn't willing to remember the whole debt.
        return query_yn(
                   _( "I'm never going to be able to pay you back for all that.  The most I'm "
                      "willing to owe you is %s.\n\nContinue with trade?" ),
                   format_money( np.max_willing_to_owe() ) );

    } else {
        return query_yn( _( "Looks like a deal!  Accept this trade?" ) );
    }

    return false;
}

cataimgui::bounds trade_ui::get_bounds()
{
    return { 0, 0, get_window_width() * 0.7f, float( get_window_height() ) };
}


void trade_ui::draw_controls()
{
    npc const &np = *_parties[_trader]->as_npc();
    nc_color const trade_color =
        npc_trading::npc_will_accept_trade( np, _balance ) ? c_green : c_red;
    std::string cost_str = _( "Exchange" );
    if( !np.will_exchange_items_freely() ) {
        cost_str = string_format( _balance >= 0 ? _( "Credit %s" ) : _( "Debt %s" ),
                                  format_money( std::abs( _balance ) ) );
    }
    draw_colored_text( cost_str, trade_color, cataimgui::text_align::Center );
    draw_colored_text( _parties[_trader]->get_name(), c_white );
    ImGui::SameLine();
    draw_colored_text( _( "You" ), c_white, cataimgui::text_align::Right );
    draw_colored_text( string_format( _( "%s to switch panes" ),
                                      colorize( _panes[_you]->get_ctxt()->get_desc(
                                              trade_selector::ACTION_SWITCH_PANES ),
                                              c_yellow ) ), c_white, cataimgui::text_align::Center );
    draw_colored_text( string_format( _( "%s to auto balance with highlighted item" ),
                                      colorize( _panes[_you]->get_ctxt()->get_desc(
                                              trade_selector::ACTION_AUTOBALANCE ),
                                              c_yellow ) ), c_white, cataimgui::text_align::Center );
    ImVec2 size_temp = ImGui::GetWindowSize();
    if( size_temp.x != window_size.x || size_temp.y != window_size.y ) {
        resize();
        window_size.x = int( size_temp.x );
        window_size.y = int( size_temp.y );
    }
    //_panes[0]->draw();
    //ImGui::SameLine( 0, 0 );
    //_panes[1]->draw();
}

trade_selector::trade_selector( trade_ui *parent, Character &u,
                                inventory_selector_preset const &preset,
                                std::string const &selection_column_title )
    : inventory_drop_selector( parent, u, preset, selection_column_title ), _parent( parent ),
      _ctxt_trade( "INVENTORY", keyboard_mode::keychar )
{
    window_flags = ImGuiWindowFlags_None;
    _ctxt_trade.register_action( ACTION_SWITCH_PANES );
    _ctxt_trade.register_action( ACTION_TRADE_CANCEL );
    _ctxt_trade.register_action( ACTION_TRADE_OK );
    _ctxt_trade.register_action( ACTION_AUTOBALANCE );
    _ctxt_trade.register_action( "UP" );
    _ctxt_trade.register_action( "DOWN" );
    _ctxt_trade.register_action( "SELECT" );

    // register mouse motion related actions so we can update the active _cpane
    _ctxt_trade.register_action( "COORDINATE" );
    _ctxt_trade.register_action( "MOUSE_MOVE" );

    _ctxt_trade.register_action( "ANY_INPUT" );
    _ctxt_trade.set_timeout( 0 );
    // duplicate this action in the parent ctxt so it shows up in the keybindings menu
    // CANCEL and OK are already set in inventory_selector
    ctxt.register_action( ACTION_SWITCH_PANES );
    ctxt.register_action( ACTION_AUTOBALANCE );
    resize( {-1, -1, -1, -1} );
    for( inventory_column *col : columns ) {
        col->prepare_paging();
    }
    set_invlet_type( inventory_selector::SELECTOR_INVLET_ALPHA );
    show_buttons = false;
}

trade_selector::select_t trade_selector::to_trade() const
{
    return to_use;
}

void trade_selector::execute()
{
    bool exit = false;

    columns[0]->on_activate();

    while( !exit ) {
#if !(defined(WIN32) || defined(TILES))
        ui_adaptor::redraw_all_invalidated( true );
#endif
        std::string const &action = _ctxt_trade.handle_input();
        if( !is_open ) {
            break;
        }
        if( action == ACTION_SWITCH_PANES ) {
            _parent->pushevent( trade_ui::event::SWITCH );
            columns[active_column_index]->on_deactivate();
            exit = true;
        } else if( action == ACTION_TRADE_OK ) {
            _parent->pushevent( trade_ui::event::TRADEOK );
            exit = true;
        } else if( action == ACTION_TRADE_CANCEL || !is_open ) {
            _parent->pushevent( trade_ui::event::TRADECANCEL );
            exit = true;
        } else if( action == ACTION_AUTOBALANCE ) {
            _parent->autobalance();
        } else if( action == "MOUSE_MOVE" ) {
            // break out of the loop when the mouse is moved, this allows us to update the current pane
            exit = true;
        } else {
            //if((action == "UP" || action == "DOWN") && keyboard_focused_entry != nullptr && !is_mine(*keyboard_focused_entry))
            //{
            //    _parent->pushevent(trade_ui::event::SWITCH);
            //    break;
            //}
            input_event const iev = _ctxt_trade.get_raw_input();
            inventory_input const input =
                process_input( ctxt.input_to_action( iev ), iev.get_first_input() );
            inventory_drop_selector::on_input( input );
            if( input.action == "HELP_KEYBINDINGS" ) {
                ctxt.display_menu();
            }
        }
    }
}

std::vector<inventory_entry *> trade_selector::get_items()
{
    std::vector<inventory_entry *> output;
    for( inventory_column *col : columns ) {
        if( col->visible() ) {
            std::vector<inventory_entry *> entries = col->get_entries( []( const inventory_entry & ent ) {
                return ent.is_item();
            } );
            output.insert( output.end(), entries.begin(), entries.end() );
        }
    }
    return output;
}

void trade_selector::on_toggle()
{
    _parent->recalc_values_cpane();
}

void trade_selector::resize( const cataimgui::bounds &pane_bounds )
{
    _fixed_bounds.emplace( pane_bounds );
}

input_context const *trade_selector::get_ctxt() const
{
    return &_ctxt_trade;
}
