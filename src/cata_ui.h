#pragma once
#include "point.h"
#include "translation.h"
#include <map>
#include "color.h"

class ui_adaptor;
class input_context;

namespace catacurses
{
class window;
}

namespace cata_ui
{

struct action_handler {
    std::string action_id;
    translation tl;
    std::function<void()> on_action_execute;

    action_handler()
    {
    }
    action_handler( std::string &action_id, std::function<void()> on_action_execute );
    action_handler( std::string &action_id, std::function<void()> on_action_execute, translation tl );
    action_handler(const action_handler &other);
};

class fragment
{
        friend class dialog;
        friend class scroll_view;
    protected:
        std::string text;
        point location;
        nc_color color;
        bool invalidated;
        int height;
        int max_height;
        int width;
        scroll_view *parent;
        bool highlighted;
        fragment();

    public:

        bool enabled;
        bool selectable;
        int get_actual_height();
        point get_location();
        point get_scrolled_location(point offset = point_zero);
        nc_color get_color();
        std::string get_text();
        void invalidate();
        virtual void draw( catacurses::window &win ) = 0;
};

class edit_text : public fragment
{
public:
    void draw(catacurses::window &win);
};

class scroll_view : public fragment
{
    friend class dialog;
public:
    int scroll_height;
    void draw(catacurses::window &win);

    std::vector<fragment *> children;
};

class button : public fragment
{
    friend class dialog;
    std::string action;
    char mnemonic;
public:
    bool show_mnemonic;

    std::string get_action();

    std::optional<std::function<void()>> on_click;
    std::optional<std::function<void()>> on_select;

    void draw(catacurses::window &win);
};

class button_toggle : public button
{
    friend class dialog;
    int toggle_group;
};

class text_block : public fragment
{
    friend class dialog;
public:
    void draw(catacurses::window &win);
};

class dialog : fragment
{
        std::map<std::string, action_handler> action_handler_map;
        std::vector<fragment *> controls;
        std::string category;
        fragment *active;
        bool is_open;
        bool trim_width;
        void on_click_default_handler( fragment *fragment );
        void on_redraw( ui_adaptor &adaptor, catacurses::window &w );
        void draw( catacurses::window &w );

    public:
        void register_action_handler( action_handler &handler );
        dialog( std::string &category, point p, std::string title, int height, int width );
        ~dialog();

        fragment *add_button( point p, std::string &text, std::string &action );
        fragment *add_button_toggle( point p, std::string &text, std::string &action,
                                     int toggle_group_index );
        fragment *add_text_field( point p, std::string &text, int width = -1, bool selectable = false );

        fragment *add_text_multiline( point p, std::string &text, int width, int height );
        fragment *add_edit_field( point p, int width );

        fragment *get_active_fragment();

        void show();

        void close();
};
}
