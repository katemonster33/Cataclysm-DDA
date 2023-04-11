#include "cata_ui.h"
#include "ui_manager.h"
#include "input.h"

void cata_ui::dialog::register_action_handler( const std::string &action_id,
        std::function<void()> callback )
{
    action_handler_map[action_id] = { translation(), callback };
}

void cata_ui::dialog::register_action_handler( const std::string &action_id, const translation t,
        std::function<void()> callback )
{
    action_handler_map[action_id] = { t, callback };
}

void cata_ui::dialog::close()
{
    is_open = false;
}

cata_ui::dialog::dialog( std::string &category )
{
    is_open = true;
}

cata_ui::dialog::~dialog()
{
    is_open = false;
    for(cata_ui::fragment *fragment : controls)
    {
        delete fragment;
    }
}

void cata_ui::dialog::on_click_default_handler( fragment *fragment )
{
    
}

cata_ui::fragment *cata_ui::dialog::add_button( point p, std::string &text, std::string &action )
{
    return controls.emplace_back( new cata_ui::fragment(cata_ui::fragment_type::button, p, text,
        action));
}

cata_ui::fragment *cata_ui::dialog::add_button_toggle( point p, std::string &text,
        std::string &action, int toggle_group_index )
{
    return controls.emplace_back( new cata_ui::fragment( cata_ui::fragment_type::button_toggle, p, text,
                                  action ) );
}

cata_ui::fragment *cata_ui::dialog::add_text_field( point p, std::string &text, int width,
        bool selectable )
{
    cata_ui::fragment* fragment = controls.emplace_back( new cata_ui::fragment(cata_ui::fragment_type::text, p, text));
    fragment->selectable = selectable;
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

void cata_ui::dialog::on_redraw(ui_adaptor *adaptor)
{
    for(fragment *ctrl : controls)
    {

    }
}

void cata_ui::dialog::show()
{
    ui_adaptor adaptor;
    input_context ctx( category );
    for(std::pair<std::string, std::pair<translation, std::function<void()>>> kv : action_handler_map)
    {
        ctx.register_action(kv.first, kv.second.first);
    }
    adaptor.on_redraw( [this]( ui_adaptor & adaptor ) {
        on_redraw(&adaptor);
    } );

    while( is_open ) {
        const std::string input = ctx.handle_input();

        auto handled_input = action_handler_map.find(input);
        if(handled_input != action_handler_map.end())
        {
            handled_input->second.second();
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
    if(on_click)
    {
        on_click.value();
        return true;
    }
    return false;
}
