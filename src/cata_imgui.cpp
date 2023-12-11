#include "cata_imgui.h"
#include <stack>
#include "output.h"
#include "ui_manager.h"
#include "color.h"
#include "input.h"
#include <type_traits>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#if !(defined(TILES) || defined(WIN32))
#include "color_loader.h"

struct RGBTuple {
    uint8_t Blue;
    uint8_t Green;
    uint8_t Red;
};

struct pairs {
    short FG;
    short BG;
};

std::array<RGBTuple, color_loader<RGBTuple>::COLOR_NAMES_COUNT> rgbPalette;
std::array<pairs, 100> colorpairs;   //storage for pair'ed colored

void cataimgui::load_colors()
{

    color_loader<RGBTuple>().load( rgbPalette );
}

void cataimgui::init_pair( int p, int f, int b )
{
    colorpairs[p].FG = f;
    colorpairs[p].BG = b;
}

template<>
RGBTuple color_loader<RGBTuple>::from_rgb( const int r, const int g, const int b )
{
    RGBTuple result;
    // Blue
    result.Blue = b;
    // Green
    result.Green = g;
    // Red
    result.Red = r;
    return result;
}
#else
#include "sdl_utils.h"
#endif

void cataimgui::window::draw_colored_text( std::string const &text, const nc_color &color,
        text_align alignment, float max_width, bool *is_selected, bool *is_focused, bool *is_hovered )
{
    nc_color color_cpy = color;
    draw_colored_text( text, color_cpy, alignment, max_width, is_selected, is_focused, is_hovered );
}

void cataimgui::window::draw_colored_text( std::string const &text, nc_color &color,
        text_align alignment, float max_width, bool *is_selected, bool *is_focused, bool *is_hovered )
{
    ImGui::PushID( text.c_str() );
    ImGuiID itemId = GImGui->CurrentWindow->IDStack.back();
    ImGui::PushTextWrapPos( max_width );
    const auto color_segments = split_by_color( text );
    std::stack<nc_color> color_stack;
    color_stack.push( color );
    if( alignment != text_align::Left ) {
        std::string str_raw = remove_color_tags( text );
        int fullWidth = ImGui::GetContentRegionAvail().x;
        float textWidth = ImGui::CalcTextSize( str_raw.c_str() ).x;
        if( alignment == text_align::Right ) {
            ImGui::SetCursorPosX( ImGui::GetCursorPosX() + fullWidth - textWidth - 2 );
        } else if( alignment == text_align::Center ) {
            ImGui::SetCursorPosX( ImGui::GetCursorPosX() + ( float( fullWidth ) / 2 ) - ( textWidth / 2 ) );
        }
    }
    if( is_selected ) {
        ImGui::Selectable( "", is_selected );
        ImGui::SameLine( 0, 0 );
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
#if !(defined(TILES) || defined(WIN32))
        int pair_id = color.get_index();
        pairs &pair = colorpairs[pair_id];

        int palette_index = pair.FG != 0 ? pair.FG : pair.BG;
        if( color.is_bold() ) {
            palette_index += color_loader<RGBTuple>::COLOR_NAMES_COUNT / 2;
        }
        RGBTuple &rgbCol = rgbPalette[palette_index];
        ImGui::TextColored( { static_cast<float>( rgbCol.Red / 255. ), static_cast<float>( rgbCol.Green / 255. ),
                              static_cast<float>( rgbCol.Blue / 255. ), static_cast<float>( 255. ) },
                            "%s", seg.c_str() );
        GImGui->LastItemData.ID = itemId;
#else
        SDL_Color c = curses_color_to_SDL( color );
        ImGui::TextColored( { static_cast<float>( c.r / 255. ), static_cast<float>( c.g / 255. ),
                              static_cast<float>( c.b / 255. ), static_cast<float>( c.a / 255. ) },
                            "%s", seg.c_str() );
        GImGui->LastItemData.ID = itemId;
#endif
        if( is_focused && !*is_focused ) {
            *is_focused = ImGui::IsItemFocused();
        }
        if( is_hovered && !*is_hovered ) {
#if defined(TILES) || defined(WIN32)
            *is_hovered = ImGui::IsItemHovered( ImGuiHoveredFlags_NoNavOverride );
#else
            *is_hovered = ImGui::IsItemHovered( );
#endif
        }

    }

    ImGui::PopTextWrapPos();
    ImGui::PopID();
}

int cataimgui::window::draw_item_info_data( item_info_data &data )
{
    std::string buffer;
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

    if( *data.ptr_selected < 0 ) {
        *data.ptr_selected = 0;
    }

    const auto redraw = [this, data, buffer]() {
        if( !data.without_getch ) {
            if( !ImGui::Begin( data.get_item_name().c_str() ) ) {
                ImGui::End();
                return;
            }
        }
        std::stringstream sstr( buffer );
        std::string line;
        while( std::getline( sstr, line ) ) {
            if( line == "--" ) {
                ImGui::Separator();
            } else {
                draw_colored_text( line, c_light_gray );
            }
        }
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

bool cataimgui::window::get_is_open() const
{
    return is_open;
}

void cataimgui::window::set_title( const std::string &title )
{
    id = title;
}

void cataimgui::window::draw_header( std::string const &text )
{
#if !(defined(TILES) || defined(WIN32))
    ImGui::Text( "%s", text.c_str() );
#else
    ImGui::SeparatorText( text.c_str() );
#endif
}

bool cataimgui::window::is_child_window_navigated()
{
    return GImGui->CurrentWindow->ChildId == GImGui->NavId;
}

class cataimgui::window_impl : public ui_adaptor
{
        friend class cataimgui::window;
        cataimgui::window *win_base;
        bool is_resized;
    public:
        explicit window_impl( cataimgui::window *win ) {
            win_base = win;
            is_resized = true;
        }

        void redraw() override {
            win_base->draw();
        }

        void resized() override {
            is_resized = true;
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

cataimgui::window::window( const std::string &title, int window_flags ) : window( window_flags )
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
    delete p_impl;
}

bool cataimgui::window::is_resized()
{
    if( parent ) {
        return parent->is_resized();
    } else {
        return p_impl->is_resized;
    }
}

void cataimgui::window::draw()
{
    if( !is_open ) {
        return;
    }
    bool handled_resize = false;
    if( is_resized() ) {
        cached_bounds = get_bounds();
        // we want to make sure is_resized is able to be handled for at least a full frame
        handled_resize = true;
    }
    if( parent != nullptr ) {
        if( cached_bounds.x >= 0 ) {
            ImGui::SetCursorPosX( cached_bounds.x );
        }
        if( cached_bounds.y >= 0 ) {
            ImGui::SetCursorPosY( cached_bounds.y );
        }
        if( ImGui::BeginChild( id.c_str(), { cached_bounds.w, cached_bounds.h }, false, window_flags ) ) {
            draw_controls();
        }
        ImGui::EndChild();
    } else {
        if( cached_bounds.x >= 0 && cached_bounds.y >= 0 ) {
            ImGui::SetNextWindowPos( { cached_bounds.x, cached_bounds.y } );
        }
        if( cached_bounds.h > 0 && cached_bounds.w > 0 ) {
            ImGui::SetNextWindowSize( { cached_bounds.w, cached_bounds.h } );
        }
        if( ImGui::Begin( id.c_str(), &is_open, window_flags ) ) {
            draw_controls();
            if( active_popup ) {
                if( open_popup_requested ) {
                    active_popup->open();
                }
                if( active_popup->is_open ) {
                    active_popup->draw();
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
    if( handled_resize && !parent ) {
        p_impl->is_resized = false;
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
    } while( this->active_popup && this->active_popup->get_result() == cataimgui::dialog_result::None );
    if( this->active_popup ) {
        last_popup_result = this->active_popup->get_result();
        this->active_popup.reset();
    }
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

static ImGuiID popup_id = 0;
cataimgui::popup::popup( const std::string &id, bool is_modal )
{
    this->id = id;
    result = cataimgui::dialog_result::None;
    this->is_modal = is_modal;
    if( !popup_id ) {
        popup_id = ImHashStr( "POPUP" );
    }
}

cataimgui::popup::popup( const std::string &id, bool is_modal,
                         const std::function<bool()> &on_draw_callback ) : popup( id, is_modal )
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
#if defined(TILES) || defined(WIN32)
    ImGui::SetNextWindowSize( { 400, 0 } );
#else
    ImGui::SetNextWindowSize( { 50, 0 } );
#endif
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
    input.fill( 0 ); // terminate our input buffer
    this->id = title;
    this->prompt = prompt;
}

cataimgui::dialog_result cataimgui::string_input_box::show( const std::string &prompt,
        std::string &input )
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
    return std::string( input.begin(), input.end() );
}

void cataimgui::string_input_box::draw_controls()
{
    ImGui::Text( "%s", prompt.c_str() );
    ImGui::SameLine();
    if( !ImGui::IsAnyItemActive() ) {
        ImGui::SetKeyboardFocusHere( 0 );
    }
    ImGui::InputText( "##inputtext", input.data(), input.max_size() );
    if( ImGui::Button( "OK" ) || ImGui::IsKeyDown( ImGuiKey_Enter ) ) {
        result = dialog_result::OKClicked;
    }
    ImGui::SameLine();
    if( ImGui::Button( "Cancel" ) || ImGui::IsKeyDown( ImGuiKey_Escape ) ) {
        result = dialog_result::CancelClicked;
    }
}


cataimgui::list_selector::list_selector( const std::string &id ) : cataimgui::popup( id, true )
{
}

void cataimgui::list_selector::add( const cataimgui::list_selector::litem &it )
{
    items.push_back( it );
}

void cataimgui::list_selector::add( std::initializer_list<cataimgui::list_selector::litem> &items )
{
    this->items.insert( this->items.end(), items.begin(), items.end() );
}

int cataimgui::list_selector::get_selected_index() const
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
