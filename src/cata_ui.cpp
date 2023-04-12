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
    b.x = p.x;
    b.y = p.y;
    b.h = height;
    b.w = width;
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
    point scrolled_location;// = get_scrolled_location();
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

cata_ui::fragment *cata_ui::dialog::get_active_fragment()
{
    return active;
}

void cata_ui::text_block::draw( catacurses::window &win, bounds &box, bool pretend)
{
    int draw_x = 0;
    if(b.x) draw_x = b.x.value();
    else if(box.x) draw_x = box.x.value();

    int draw_y = 0;
    if(b.y) draw_y = b.y.value();
    else if(box.y) draw_y = box.y.value();

    int draw_height = 0;
    if(b.h) draw_height = b.h.value();
    else if(box.h) draw_height = box.h.value();

    int draw_width = 0;
    if(b.w) draw_width = b.w.value();
    else if(box.w) draw_width = box.w.value();

    int max_w = 0;

    if(draw_width) // is our width constrained?
    {
        if(draw_height > 1)
        {
            std::vector<std::string> splitstr = foldstring(text, draw_width);
            for(int i = 0; i < draw_height && i < splitstr.size(); i++)
            {
                print_colored_text(win, point(draw_x, draw_y+ i), color, color, splitstr[i]);
            }
        }
        else
        {
            trim_and_print(win, point(draw_x, draw_y), draw_width, color, text);
            max_w = draw_width;
        }
    }
    else // if not, just print until we reach the end of the window
    {
        std::string no_color = remove_color_tags(text);
        if(no_color.length() > max_w)
        {
            draw_width = text.length();
        }
        print_colored_text(win, point(draw_x, draw_y), color, color, text);
    }

    box.h = draw_height;
    box.w = draw_width;
}

void cata_ui::dialog::draw(catacurses::window &w, bounds &box, bool pretend)
{
    if( this->invalidated ) {
        werase( w );
        draw_border( w, BORDER_COLOR, text );
    }
    for( fragment *ctrl : controls ) {
        if( ctrl->invalidated || this->invalidated ) {
            ctrl->draw( w, box, pretend );
            ctrl->invalidated = false;
        }
    }
    this->invalidated = false;
}

void cata_ui::dialog::on_redraw( ui_adaptor &adaptor, catacurses::window &w )
{
    cata_ui::bounds b;
    b.x = 0;
    b.y = 0;
    b.w = catacurses::getmaxx(w);
    b.h = catacurses::getmaxy(w);
    draw( w, b, false );
}

void cata_ui::dialog::show()
{
    ui_adaptor adaptor;
    catacurses::window w_dialog = catacurses::newwin( b.h.value(), b.w.value(), point(b.x.value(), b.y.value()));

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
    parent = nullptr;
    this->enabled = true;
    this->invalidated = true;
}

cata_ui::fragment &cata_ui::fragment::x(int x)
{
    b.x = x;
    return *this;
}

cata_ui::fragment &cata_ui::fragment::y(int y)
{
    b.y = y;
    return *this;
}

cata_ui::fragment &cata_ui::fragment::h(int h)
{
    b.h = h;
    return *this;
}

cata_ui::fragment &cata_ui::fragment::w(int w)
{
    b.w = w;
    return *this;
}

nc_color cata_ui::fragment::get_color()
{
    if( highlighted ) {
        return hilite( color );
    } else {
        return color;
    }
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

cata_ui::scroll_view::scroll_view(cata_ui::fragment *child)
{
    this->child = child;
    cata_assert(this->child != nullptr);
}

void cata_ui::scroll_view::draw( catacurses::window &win, cata_ui::bounds &box, bool pretend )
{
    int draw_h = TERMY;
    if(box.h) draw_h = box.h.value();
    if(!box.w || !box.h)
    {
        child->draw(win, box, true);
        cata_assert(box.w && box.h);
    }
    int content_height = box.h.value();
    draw_scrollbar( win, scroll_height, height, content_height, location );
    box.x = box.x.value()++;

    for( fragment *child : children ) {
        if( child->invalidated || invalidated ) {
            child->draw( win );
            child->invalidated = false;
        }
    }
    invalidated = false;
}
