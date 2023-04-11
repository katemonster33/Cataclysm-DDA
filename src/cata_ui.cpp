#include "cata_ui.h"
#include "ui_manager.h"
#include "cursesdef.h"
#include "input.h"
#include "output.h"

cata_ui::action_handler::action_handler( std::string &action_id,
        std::function<void()> on_action_execute )
{
    this->action_id = action_id;
    this->on_action_execute = on_action_execute;
}

cata_ui::action_handler::action_handler( std::string &action_id,
        std::function<void()> on_action_execute, translation tl )
{
    this->action_id = action_id;
    this->on_action_execute = on_action_execute;
    this->tl = tl;
}

cata_ui::action_handler::action_handler( const cata_ui::action_handler &other )
{
    this->action_id = other.action_id;
    this->on_action_execute = other.on_action_execute;
    this->tl = other.tl;
}

void cata_ui::dialog::register_action_handler( action_handler &handler )
{
    action_handler_map[handler.action_id] = handler;
}

void cata_ui::dialog::close()
{
    is_open = false;
}

cata_ui::dialog::dialog( std::string &category, point p, std::string title, int height,
                         int width ) : fragment()
{
    location = p;
    this->height = height;
    this->width = width;
    this->text = title;
    is_open = true;
}

cata_ui::dialog::~dialog()
{
    is_open = false;
    for( cata_ui::fragment *fragment : controls ) {
        delete fragment;
    }
}

void cata_ui::dialog::on_click_default_handler( fragment *fragment )
{

}

void cata_ui::button::draw( catacurses::window &win )
{
    point scrolled_location = get_scrolled_location();
    if( scrolled_location.y >= 0 ) {
        std::string btn_text = string_format( "<%s>", this->text );

        if( show_mnemonic ) {
            size_t mnemonic_idx = btn_text.find( mnemonic );
            std::string mnemonic_colorized_str = colorize( std::string( 1, mnemonic ), get_color() );
            if( mnemonic_idx != -1 ) {
                btn_text.erase( btn_text.begin() + mnemonic_idx );
                btn_text.insert( mnemonic_idx, mnemonic_colorized_str );
            } else {
                btn_text.insert( 1, mnemonic_colorized_str );
            }
        }
        nc_color color_cpy = color;
        print_colored_text( win, scrolled_location, color, color, btn_text );
    }
}


cata_ui::fragment *cata_ui::dialog::add_button( point p, std::string &text, std::string &action )
{
    cata_ui::button *fragment = new cata_ui::button;

    fragment->location = p;
    fragment->text = text;
    fragment->action = action;
    controls.push_back( fragment );
    return fragment;
}

cata_ui::fragment *cata_ui::dialog::add_button_toggle( point p, std::string &text,
        std::string &action, int toggle_group_index )
{
    cata_ui::button *fragment = new cata_ui::button;

    fragment->location = p;
    fragment->text = text;
    fragment->action = action;
    controls.push_back( fragment );
    return fragment;
}

cata_ui::fragment *cata_ui::dialog::add_text_field( point p, std::string &text, int width,
        bool selectable )
{
    cata_ui::text_block *fragment = new cata_ui::text_block;

    fragment->location = p;
    fragment->text = text;
    fragment->selectable = selectable;
    controls.push_back( fragment );
    return fragment;
}

cata_ui::fragment *cata_ui::dialog::add_text_multiline( point p, std::string &text, int width,
        int height )
{
    return nullptr;
}

cata_ui::fragment *cata_ui::dialog::add_edit_field( point p, int width )
{
    return nullptr;
}

cata_ui::fragment *cata_ui::dialog::get_active_fragment()
{
    return active;
}

void cata_ui::text_block::draw( catacurses::window &win )
{
    point scrolled_location = get_scrolled_location();
    if( scrolled_location.y >= 0 ) {
        if( width ) {
            trim_and_print( win, scrolled_location, width, color, text );
        } else {
            print_colored_text( win, scrolled_location, color, color, text );
        }
    }
}

void cata_ui::dialog::draw( catacurses::window &w )
{
    if( this->invalidated ) {
        werase( w );
        draw_border( w, BORDER_COLOR, text );
    }
    for( fragment *ctrl : controls ) {
        if( ctrl->invalidated || this->invalidated ) {
            ctrl->draw( w );
            ctrl->invalidated = false;
        }
    }
    this->invalidated = false;
}

void cata_ui::dialog::on_redraw( ui_adaptor &adaptor, catacurses::window &w )
{
    draw( w );
}

void cata_ui::dialog::show()
{
    ui_adaptor adaptor;
    catacurses::window w_dialog = catacurses::newwin( height, width, location );

    input_context ctx( category );
    for( std::pair<const std::string, cata_ui::action_handler> kv : action_handler_map ) {
        ctx.register_action( kv.second.action_id, kv.second.tl );
    }
    adaptor.on_redraw( [&]( ui_adaptor & adaptor ) {
        on_redraw( adaptor, w_dialog );
    } );

    while( is_open ) {
        const std::string input = ctx.handle_input();

        auto handled_input = action_handler_map.find( input );
        if( handled_input != action_handler_map.end() ) {
            handled_input->second.on_action_execute();
        }

        adaptor.redraw_invalidated();
    }
}

cata_ui::fragment::fragment()
{
    width = 0;
    height = 1;
    max_height = 1;
    parent = nullptr;
    this->enabled = true;
    this->invalidated = true;
}

point cata_ui::fragment::get_scrolled_location(point offset)
{
    int y_offset = location.y;
    if(parent)
    {
        y_offset -= parent->location.y;
    }
    return point( location.x + offset.x, y_offset + offset.y );
}

nc_color cata_ui::fragment::get_color()
{
    if( highlighted ) {
        return hilite( color );
    } else {
        return color;
    }
}

point cata_ui::fragment::get_location()
{
    return location;
}

std::string cata_ui::fragment::get_text()
{
    return text;
}

std::string cata_ui::button::get_action()
{
    return action;
}

void cata_ui::fragment::invalidate()
{
    invalidated = true;
}

/// <summary>
/// meant to be overridden by any classes which could have heights of > 1 or variable determined by their contents
/// </summary>
/// <returns></returns>
int cata_ui::fragment::get_actual_height()
{
    return height;
}

void cata_ui::scroll_view::draw( catacurses::window &win )
{
    if( invalidated ) {
        int content_height = 0;
        for( fragment *child : children ) {
            int tmp_height = child->get_location().y + child->get_actual_height();
            if( tmp_height > content_height ) {
                content_height = tmp_height;
            }
        }
        draw_scrollbar( win, scroll_height, height, content_height, location );
    }
    for( fragment *child : children ) {
        if( child->invalidated || invalidated ) {
            child->draw( win );
            child->invalidated = false;
        }
    }
    invalidated = false;
}
