#include "cata_imgui.h"
#include <stack>
#include "output.h"
#include "ui_manager.h"
#include "color.h"
#include "input.h"
#include <type_traits>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#include <SDL2/SDL_pixels.h>
#include "sdl_utils.h"

void cataimgui::window::draw_colored_text( std::string const &text, const nc_color &color,
        text_align alignment )
{
    nc_color color_cpy = color;
    draw_colored_text( text, color_cpy, alignment );
}

void cataimgui::window::draw_colored_text( std::string const &text, nc_color &color,
        text_align alignment )
{
    const auto color_segments = split_by_color( text );
    std::stack<nc_color> color_stack;
    color_stack.push( color );
    if( alignment != text_align::Left ) {
        std::string str_raw = remove_color_tags( text );
        int fullWidth = ImGui::GetContentRegionAvail().x;
        auto textWidth = ImGui::CalcTextSize( str_raw.c_str() ).x;
        if( alignment == text_align::Right ) {
            ImGui::SetCursorPosX( ImGui::GetCursorPosX() + fullWidth - textWidth - 2 );
        } else if( alignment == text_align::Center ) {
            ImGui::SetCursorPosX( ImGui::GetCursorPosX() + ( fullWidth / 2 ) - ( textWidth / 2 ) );
        }
    }

    int i = 0;
    for( auto seg : color_segments ) {
        if( seg.empty() ) {
            continue;
        }

        if( seg[0] == '<' ) {
            const color_tag_parse_result::tag_type type =
                update_color_stack( color_stack, seg, report_color_error::yes );
            if( type != color_tag_parse_result::non_color_tag ) {
                seg = rm_prefix( seg );
            }
        }

        color = color_stack.empty() ? color : color_stack.top();
        if( i++ > 0 ) {
            ImGui::SameLine( 0, 0 );
        }
        SDL_Color c = curses_color_to_SDL( color );
        ImGui::TextColored( { static_cast<float>( c.r / 255. ), static_cast<float>( c.g / 255. ),
                              static_cast<float>( c.b / 255. ), static_cast<float>( c.a / 255. ) },
                            "%s", seg.c_str() );
    }
}

int cataimgui::window::draw_item_info_data( item_info_data &data )
{
    std::string buffer;
    int line_num = data.use_full_win || data.without_border ? 0 : 1;
    if( !data.get_item_name().empty() ) {
        buffer += data.get_item_name() + "\n";
    }
    // If type name is set, and not already contained in item name, output it too
    if( !data.get_type_name().empty() &&
        data.get_item_name().find( data.get_type_name() ) == std::string::npos ) {
        buffer += data.get_type_name() + "\n";
    }
    for( unsigned int i = 0; i < data.padding; i++ ) {
        buffer += "\n";
    }

    buffer += format_item_info( data.get_item_display(), data.get_item_compare() );

    const int b = data.use_full_win ? 0 : ( data.without_border ? 1 : 2 );
    int num_lines = 0;
    if( *data.ptr_selected < 0 ) {
        *data.ptr_selected = 0;
    }

    const auto redraw = [this, data, &num_lines, buffer]() {
        num_lines = 0;
        if( !data.without_getch ) {
            if( !ImGui::Begin( data.get_item_name().c_str() ) ) {
                ImGui::End();
                return;
            }
        }
        std::stringstream sstr( buffer );
        std::string line;
        while( std::getline( sstr, line ) ) {
            num_lines++;
            if( line == "--" ) {
                ImGui::Separator();
            } else {
                draw_colored_text( line, c_light_gray );
            }
        }
        //if( !data.without_border ) {
        //    draw_custom_border( win, buffer.empty() );
        //}
        if( !data.without_getch ) {
            ImGui::End();
        }
    };

    if( data.without_getch ) {
        redraw();
        return 0;
    }

    input_context ctxt( "default", keyboard_mode::keychar );
    if( data.handle_scrolling ) {
        ctxt.register_action( "PAGE_UP" );
        ctxt.register_action( "PAGE_DOWN" );
    }
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    if( data.any_input ) {
        ctxt.register_action( "ANY_INPUT" );
    }

    std::string action;
    while( true ) {
        redraw();
        action = ctxt.handle_input();

        if( action == "CONFIRM" || action == "QUIT" ||
            ( data.any_input && action == "ANY_INPUT" &&
              !ctxt.get_raw_input().sequence.empty() ) ) {
            break;
        }
    }

    return ctxt.get_raw_input().get_first_input();
}

bool cataimgui::window::get_is_open()
{
    return is_open;
}

void cataimgui::window::set_title( const std::string &title )
{
    id = title;
}

void cataimgui::window::draw_header( std::string const &text )
{
    ImGui::SeparatorText( text.c_str() );
}

bool cataimgui::window::is_child_window_navigated()
{
    return GImGui->CurrentWindow->ChildId == GImGui->NavId;
}

class cataimgui::window_impl : public ui_adaptor
{
        cataimgui::window *win_base;
    public:
        window_impl( cataimgui::window *win ) : ui_adaptor() {
            win_base = win;
        }

        void redraw() override {
            win_base->draw();
        }
};

cataimgui::window::window( int window_flags )
{
    p_impl = nullptr;
    last_popup_result = dialog_result::None;

    this->window_flags = window_flags | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoSavedSettings;
    parent = nullptr;
}

cataimgui::window::window( cataimgui::window *parent, int window_flags ) : window( window_flags )
{
    this->parent = parent;
    this->parent->add_child( this );
    is_open = true;
}

cataimgui::window::window( std::string title, int window_flags ) : window( window_flags )
{
    p_impl = new cataimgui::window_impl( this );
    p_impl->is_imgui = true;
    id = title;
    is_open = true;
}

cataimgui::window::~window()
{
    for( cataimgui::window *child : children ) {
        child->is_open = false;
    }
    if( p_impl ) {
        delete p_impl;
    }
}

void cataimgui::window::draw()
{
    if( !is_open ) {
        return;
    }
    bounds b = get_bounds();
    if( parent != nullptr ) {
        if( b.x >= 0 ) {
            ImGui::SetCursorPosX( b.x );
        }
        if( b.y >= 0 ) {
            ImGui::SetCursorPosY( b.y );
        }
        if( ImGui::BeginChild( id.c_str(), { b.w, b.h }, false, window_flags ) ) {
            draw_controls();
        }
        ImGui::EndChild();
    } else {
        if( b.x >= 0 && b.y >= 0 ) {
            ImGui::SetNextWindowPos( { b.x, b.y } );
        }
        if( b.h > 0 && b.w > 0 ) {
            ImGui::SetNextWindowSize( { b.w, b.h } );
        }
        if( ImGui::Begin( id.c_str(), &is_open, window_flags ) ) {
            draw_controls();
            if( active_popup ) {
                if( open_popup_requested ) {
                    active_popup.get()->open();
                }
                if( active_popup.get()->is_open ) {
                    active_popup.get()->draw();
                } else {
                    active_popup.reset();
                }
            }
            for( window *child : children ) {
                child->draw();
            }
        }
        ImGui::End();
    }
}

/// <summary>
/// This method schedules a popup to be drawn on the next ImGui draw frame, the popup code is handled alongside the current window so the current window can continue processing inputs.
///  Use this function if opening a popup from inside a draw_controls function
/// </summary>
/// <param name="next_popup">the popup to be shown</param>
void cataimgui::window::show_popup_async( const std::shared_ptr<popup> &next_popup )
{
    this->active_popup = next_popup;
}

void cataimgui::window::show_popup_async( popup *next_popup )
{
    this->active_popup.reset( next_popup );
}

/// <summary>
/// This method shows a popup and blocks until the popup closes. This will crash if called from inside draw_controls
/// </summary>
/// <param name="next_popup">the popup to show</param>
/// <returns></returns>
cataimgui::dialog_result cataimgui::window::show_popup( const std::shared_ptr<popup> &next_popup )
{
    this->active_popup = next_popup;
    // on the next draw(), open the popup.
    open_popup_requested = true;
    input_context tmp_context( "IMGUI_WINDOW_POPUP" );
    tmp_context.set_timeout( 0 );
    tmp_context.register_action( "ANY_INPUT" );
    do {
        // force a redraw
        tmp_context.handle_input();
    } while( this->active_popup != nullptr );
    return last_popup_result;
}

cataimgui::dialog_result cataimgui::window::show_popup( popup *next_popup )
{
    return show_popup( std::shared_ptr<popup>( next_popup ) );
}

/// <summary>
/// A button tied to a string action. When the button is clicked, push the desired action to input_context to be returned the next time
///  there is no input action
/// </summary>
/// <param name="action">The action id to be returned by the button</param>
/// <param name="text">The button's text</param>
/// <returns></returns>
bool cataimgui::window::action_button( const std::string &action, const std::string &text )
{
    if( ImGui::Button( text.c_str() ) ) {
        input_context::set_action_override( action );
        return true;
    }
    return false;
}

cataimgui::bounds cataimgui::window::get_bounds()
{
    return { -1, -1, -1, -1 };
}

void cataimgui::window::add_child( cataimgui::window *child )
{
    children.push_back( child );
}

bool cataimgui::is_drag_drop_active()
{
    return ImGui::GetCurrentContext()->DragDropActive;
}

ImGuiID popup_id = 0;
cataimgui::popup::popup( std::string id, bool is_modal ) : cataimgui::window( )
{
    this->id = id;
    result = cataimgui::dialog_result::None;
    this->is_modal = is_modal;
    if( !popup_id ) {
        popup_id = ImHashStr( "POPUP" );
    }
}

cataimgui::popup::popup( std::string id, bool is_modal,
                         std::function<bool()> on_draw_callback ) : popup( id, is_modal )
{
    this->on_draw_callback = on_draw_callback;
}

cataimgui::popup::~popup()
{
    if( is_open ) {
        close();
    }
}

void cataimgui::popup::set_draw_callback( const std::function<bool()> &callback )
{
    on_draw_callback = callback;
}

void cataimgui::popup::draw()
{
    ImGui::PushOverrideID( popup_id );
    if( is_modal ) {
        if( ImGui::BeginPopupModal( id.c_str(), &is_open, ImGuiWindowFlags_AlwaysAutoResize ) ) {
            draw_controls();
            ImGui::EndPopup();
        }
    } else {
        if( ImGui::BeginPopup( id.c_str() ) ) {
            draw_controls();
            ImGui::EndPopup();
        }
    }
    ImGui::PopID();
    if( on_draw_callback ) {
        on_draw_callback();
    }
}

void cataimgui::popup::open()
{
    is_open = true;
    ImGui::PushOverrideID( popup_id );
    ImGui::OpenPopup( id.c_str() );
    ImGui::PopID();
}

void cataimgui::popup::close()
{
    is_open = false;
    ImGui::CloseCurrentPopup();
}

cataimgui::dialog_result cataimgui::popup::get_result()
{
    return result;
}

bool cataimgui::popup::is_draw_callback_set()
{
    return bool( on_draw_callback );
}

cataimgui::message_box::message_box( const std::string &title,
                                     const std::string &prompt, cataimgui::mbox_btn buttons ) : cataimgui::popup( title,
                                                 true )
{
    this->buttons = buttons;
    this->prompt = prompt;
}

cataimgui::dialog_result cataimgui::message_box::show( const std::string &title,
        const std::string &text )
{
    input_context ctx( "INPUT_BOX" );
    ctx.register_action( "ANY_INPUT" );
    message_box input_box( title, text );
    while( input_box.get_result() == dialog_result::None ) {
        ctx.handle_input();
    }
    return input_box.get_result();
}

void cataimgui::message_box::draw_mbox_btn( const std::string &text,
        dialog_result result_if_clicked )
{
    if( ImGui::Button( text.c_str() ) ) {
        result = result_if_clicked;
    }
}

void cataimgui::message_box::draw_controls()
{
    ImGui::Indent( 1.0f );
    nc_color tcolor = c_light_gray;
    draw_colored_text( prompt, tcolor );
    ImGui::Unindent( 1.0f );
    if( ImGui::IsKeyDown( ImGuiKey_Escape ) ) {
        if( buttons == mbox_btn::OKCancel || buttons == mbox_btn::YesNoCancel ) {
            result = dialog_result::CancelClicked;
        } else if( buttons == mbox_btn::YesNo ) {
            result = dialog_result::NoClicked;
        } else if( buttons == mbox_btn::OK ) {
            result = dialog_result::OKClicked;
        }
    }
    if( buttons == mbox_btn::OK || buttons == mbox_btn::OKCancel ) {
        draw_mbox_btn( _( "OK" ), dialog_result::OKClicked );
        ImGui::SameLine();
    }
    if( buttons == mbox_btn::YesNo || buttons == mbox_btn::YesNoCancel ) {
        draw_mbox_btn( _( "Yes" ), dialog_result::YesClicked );
        ImGui::SameLine();
        draw_mbox_btn( _( "No" ), dialog_result::NoClicked );
        ImGui::SameLine();
    }
    if( buttons == mbox_btn::YesNoCancel || buttons == mbox_btn::OKCancel ) {
        draw_mbox_btn( _( "Cancel" ), dialog_result::CancelClicked );
    }
    if( !is_draw_callback_set() && result != dialog_result::None ) {
        close();
    }
}

cataimgui::string_input_box::string_input_box( const std::string &title,
        const std::string &prompt ) : popup( title, true )
{
    input[0] = 0; // terminate our input buffer
    this->id = title;
    this->prompt = prompt;
}

cataimgui::dialog_result cataimgui::string_input_box::show( std::string prompt, std::string &input )
{
    input_context ctx( "INPUT_BOX" );
    ctx.register_action( "ANY_INPUT" );
    string_input_box input_box( "Input", prompt );
    while( input_box.get_result() == dialog_result::None ) {
        ctx.handle_input();
    }
    if( input_box.get_result() == dialog_result::OKClicked ) {
        input.assign( input_box.get_input() );
    }
    return input_box.get_result();
}

std::string cataimgui::string_input_box::get_input()
{
    return std::string( input );
}

void cataimgui::string_input_box::draw_controls()
{
    ImGui::Text( "%s", prompt.c_str() );
    ImGui::SameLine();
    if( !ImGui::IsAnyItemActive() ) {
        ImGui::SetKeyboardFocusHere( 0 );
    }
    ImGui::InputText( "##inputtext", input, std::extent< decltype( input ) >::value );
    if( ImGui::Button( "OK" ) || ImGui::IsKeyDown( ImGuiKey_Enter ) ) {
        result = dialog_result::OKClicked;
    }
    ImGui::SameLine();
    if( ImGui::Button( "Cancel" ) || ImGui::IsKeyDown( ImGuiKey_Escape ) ) {
        result = dialog_result::CancelClicked;
    }
}


cataimgui::list_selector::list_selector( std::string id ) : cataimgui::popup( id, true )
{
}

void cataimgui::list_selector::add( cataimgui::list_selector::litem it )
{
    items.push_back( it );
}

void cataimgui::list_selector::add( std::initializer_list<cataimgui::list_selector::litem> &items )
{
    this->items.insert( this->items.end(), items.begin(), items.end() );
}

int cataimgui::list_selector::get_selected_index()
{
    return selected_index;
}

void cataimgui::list_selector::draw_controls()
{
    int index_tmp = 0;
    for( cataimgui::list_selector::litem &it : items ) {
        ImGui::PushID( it.text.c_str() );
        if( it.is_enabled ) {
            ImGui::Selectable( "", &it.is_selected );
        } else {
            ImGui::Text( "%s", "" );
        }
        ImGui::SameLine( 0, 0 );
        nc_color tcolor = c_light_gray;
        draw_colored_text( it.text, tcolor );
        ImGui::PopID();
        if( it.is_selected ) {
            selected_index = index_tmp;
            result = dialog_result::OKClicked;
            close();
        }
        index_tmp++;
    }
}
