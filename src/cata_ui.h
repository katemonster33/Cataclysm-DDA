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

    action_handler() {
    }
    action_handler( std::string &action_id, std::function<void()> on_action_execute );
    action_handler( std::string &action_id, std::function<void()> on_action_execute, translation tl );
    action_handler( const action_handler &other );
};

struct bounds
{
    std::optional<int> x;
    std::optional<int> y;
    std::optional<int> h;
    std::optional<int> w;
};

class fragment
{
        friend class dialog;
        friend class scroll_view;
    protected:
        std::string text;
        nc_color color;
        bool invalidated;
        bounds b;
        scroll_view *parent;
        bool highlighted;
        fragment();

    public:

        bool enabled;
        bool selectable;
        nc_color get_color();
        std::string get_text();
        void invalidate();
        virtual void draw( catacurses::window &win, bounds &box, bool pretend ) = 0;

        fragment &x(int x);
        fragment &y(int y);
        fragment &h(int h);
        fragment &w(int w);
};

class edit_text : public fragment
{
    public:
        edit_text(int width);
        void draw( catacurses::window &win, bounds &box);
};

class scroll_view : public fragment
{
        friend class dialog;
        fragment *child;
    public:
        scroll_view(fragment *child);
        int scroll_height;
        void draw( catacurses::window &win, bounds &box, bool pretend);

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

        void draw( catacurses::window &win );
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
        void draw( catacurses::window &win, bounds &box, bool pretend);
};

enum class stack_dir
{
    down,
    left,
    up,
    right
};

class stack : public fragment
{
    friend class dialog;
    std::vector<fragment *> children;
    stack_dir dir;
public:
    stack(std::vector<fragment *> children);

    stack set_dir(stack_dir dir);

    void arrange_children();
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

    public:
        void register_action_handler( action_handler &handler );
        void draw(catacurses::window &w, bounds &box, bool pretend);
        dialog( std::string &category, point p, std::string title, int height, int width );
        ~dialog();

        fragment *get_active_fragment();

        void show();

        void close();
};
}
