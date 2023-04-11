#pragma once
#include "point.h"
#include "translation.h"
#include <map>
#include "color.h"

class ui_adaptor;
class input_context;

namespace cata_ui
{
enum class fragment_type {
    button,
    button_toggle,
    text,
    text_multiline,
    edit
};

struct action_handler {
    std::string action_id;
    translation tl;
    std::function<void()> on_action_execute;

    action_handler( std::string &action_id, std::function<void()> on_action_execute );
    action_handler( std::string &action_id, std::function<void()> on_action_execute, translation tl );
};

class fragment
{
        friend class dialog;
        fragment_type type;
        std::string text;
        std::string action;
        point location;
        bool invalidated;
        int trim_width;
        nc_color color;

        fragment();

    public:
        class dialog *parent;

        bool show_mnemonic;
        bool enabled;
        bool selectable;
        std::optional<std::function<void()>> on_click;
        std::optional<std::function<void()>> on_select;

        fragment_type get_fragment_type();
        point get_location();
        std::string get_text();
        std::string get_action();
        bool run_custom_click_handler();
        void invalidate();
};

class dialog
{
        std::map<std::string, action_handler> action_handler_map;
        std::vector<fragment *> controls;
        std::string category;
        std::string title;
        point location;
        int height;
        int width;
        fragment *active;
        bool is_open;
        bool trim_width;
        bool invalidated;
        void on_click_default_handler( fragment *fragment );
        void on_redraw( ui_adaptor& adaptor, catacurses::window& w );
        void draw( catacurses::window& w, fragment* fragment );

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
