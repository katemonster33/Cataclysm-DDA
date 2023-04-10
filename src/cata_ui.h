#pragma once
#include "ui_manager.h"

namespace cata_ui
{
    class dialog
    {
        ui_adaptor adaptor;
        class input_context* ctx;
        void on_redraw( ui_adaptor& adaptor );

    public:
        dialog(std::string& input_context);

        ui_fragmant* add_button( point p, std::string& text, std::string& action );
        ui_fragmant* add_button_toggle( point p, std::string& text, std::string& action, int toggle_group_index );
        ui_fragmant* add_text_field( point p, std::string& text, int width = -1, bool selectable = false );
        ui_fragmant* add_text_multiline( point p, std::string& text, int width, int height );
        ui_fragmant* add_edit_field( point p, int width );

        void show();
    };
    enum class ui_fragment_type {
        button,
        button_toggle,
        text,
        text_multiline,
        edit
    };
    class ui_fragmant
    {
        std::string text;
        point location;
    public:
        ui_fragmant( ui_fragment_type t, point p, std::string& text );
        ui_fragmant( ui_fragment_type t, point p, std::string& text, std::string& action );

        dialog parent;

        bool show_mnemonic;

        ui_fragment_type get_fragment_type();
        point get_location();
        std::string get_text();
    };
}
