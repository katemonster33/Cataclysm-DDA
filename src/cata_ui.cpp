#include "cata_ui.h"


void cata_ui::dialog::on_redraw(ui_adaptor &adaptor)
{

}

cata_ui::dialog::dialog(std::string &input_context)
{

}

cata_ui::ui_fragmant *cata_ui::dialog::add_button(point p, std::string &text, std::string &action)
{

}

cata_ui::ui_fragmant *cata_ui::dialog::add_button_toggle(point p, std::string &text, std::string &action, int toggle_group_index)
{

}

cata_ui::ui_fragmant *cata_ui::dialog::add_text_field(point p, std::string &text, int width, bool selectable)
{

}

cata_ui::ui_fragmant *cata_ui::dialog::add_text_multiline(point p, std::string &text, int width, int height)
{

}

cata_ui::ui_fragmant *cata_ui::dialog::add_edit_field(point p, int width)
{

}

void cata_ui::dialog::show()
{

}

cata_ui::ui_fragmant::ui_fragmant(cata_ui::ui_fragment_type t, point p, std::string &text)
{

}

cata_ui::ui_fragmant::ui_fragmant(cata_ui::ui_fragment_type t, point p, std::string &text, std::string &action)
{

}

cata_ui::ui_fragment_type cata_ui::ui_fragmant::get_fragment_type()
{

}

point cata_ui::ui_fragmant::get_location()
{
    return location;
}

std::string cata_ui::ui_fragmant::get_text()
{
    return text;
}
