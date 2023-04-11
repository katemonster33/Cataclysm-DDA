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

void cata_ui::dialog::register_action_handler( action_handler &handler )
{
    action_handler_map[handler.action_id] = handler;
}

void cata_ui::dialog::close()
{
    is_open = false;
}

cata_ui::dialog::dialog( std::string& category, point p, std::string title, int height, int width )
{
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

cata_ui::fragment *cata_ui::dialog::add_button( point p, std::string &text, std::string &action )
{
    cata_ui::fragment *fragment = new cata_ui::fragment;

    fragment->type = cata_ui::fragment_type::button;
    fragment->location = p;
    fragment->text = text;
    fragment->action = action;
    controls.push_back( fragment );
    return fragment;
}

cata_ui::fragment *cata_ui::dialog::add_button_toggle( point p, std::string &text,
        std::string &action, int toggle_group_index )
{
    cata_ui::fragment *fragment = new cata_ui::fragment;

    fragment->type = cata_ui::fragment_type::button_toggle;
    fragment->location = p;
    fragment->text = text;
    fragment->action = action;
    controls.push_back( fragment );
    return fragment;
}

cata_ui::fragment *cata_ui::dialog::add_text_field( point p, std::string &text, int width,
        bool selectable )
{
    cata_ui::fragment *fragment = new cata_ui::fragment;

    fragment->type = cata_ui::fragment_type::text;
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

void cata_ui::dialog::draw(catacurses::window &w, fragment* fragment ) {
    switch( fragment->type ) {
    case cata_ui::fragment_type::button:
    case cata_ui::fragment_type::button_toggle:
        break;
    case cata_ui::fragment_type::text:
        if(fragment->trim_width ){
            trim_and_print( w, fragment->location, fragment->trim_width, fragment->color, fragment->text );
        } else {
            print_colored_text( w, fragment->location, fragment->color, fragment->color, fragment->text );
        }
        break;
    }
}

void cata_ui::dialog::on_redraw( ui_adaptor& adaptor, catacurses::window& w )
{
    for( fragment* ctrl : controls ) {
        if( ctrl->invalidated || this->invalidated ) {
            draw( w, ctrl );
        }
    }
}

void cata_ui::dialog::show()
{
    ui_adaptor adaptor;
    catacurses::window w_dialog;

    input_context ctx( category );
    for( std::pair<std::string, action_handler> kv : action_handler_map ) {
        ctx.register_action( kv.second.action_id, kv.second.tl );
    }
    adaptor.on_redraw( [this, w_dialog]( ui_adaptor & adaptor ) {
        on_redraw( &adaptor );
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

cata_ui::fragment::fragment( cata_ui::fragment_type t, point p, std::string &text )
{
    this->type = t;
    this->location = p;
    this->text = text;
}

cata_ui::fragment::fragment( cata_ui::fragment_type t, point p, std::string &text,
                             std::string &action )
{
    this->type = t;
    this->location = p;
    this->text = text;
    this->action = action;
}

cata_ui::fragment_type cata_ui::fragment::get_fragment_type()
{
    return type;
}

point cata_ui::fragment::get_location()
{
    return location;
}

std::string cata_ui::fragment::get_text()
{
    return text;
}

std::string cata_ui::fragment::get_action()
{
    return action;
}

bool cata_ui::fragment::run_custom_click_handler()
{
    if( on_click ) {
        on_click.value();
        return true;
    }
    return false;
}

void cata_ui::fragment::invalidate()
{
    invalidated = true;
}
