#pragma once
#include "point.h"
#include "translation.h"
#include <map>

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

class fragment
{
        fragment_type type;
        std::string text;
        std::string action;
        point location;

    public:
        fragment( fragment_type t, point p, std::string &text );
        fragment( fragment_type t, point p, std::string &text, std::string &action );
        fragment( fragment_type t, point p, std::string &text, std::function<void()> custom_click_handler );
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
};

class dialog
{
        std::map<std::string, std::pair<translation, std::function<void()>>> action_handler_map;
        std::vector<fragment*> controls;
        std::string category;
        fragment *active;
        bool is_open;
        bool trim_width;

        void on_click_default_handler( fragment *fragment );
        void on_redraw(ui_adaptor *adaptor);

    public:
        void register_action_handler( const std::string &action_id, std::function<void()> callback );
        void register_action_handler( const std::string &action_id, const translation t,
                                      std::function<void()> callback );
        dialog( std::string &category );
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
