#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <array>

class nc_color;
struct item_info_data;

namespace cataimgui
{
struct bounds {
    float x;
    float y;
    float w;
    float h;
};

enum class mbox_btn {
    BT_OK = 0,
    BT_OKCancel = 1,
    BT_YesNoCancel = 2,
    BT_YesNo = 3
};

enum class dialog_result {
    None = 0,
    OKClicked,
    CancelClicked,
    YesClicked,
    NoClicked
};

enum class text_align {
    Left = 0,
    Center = 1,
    Right = 2
};

class client
{
    public:
        client();
        ~client();

        void new_frame();
        void end_frame();
        void process_input( void *input );
#if !(defined(TILES) || defined(WIN32))
        void upload_color_pair( int p, int f, int b );
        void set_alloced_pair_count( short count );
#endif
};

class window
{
        friend class child_window;
        class window_impl *p_impl;
        std::shared_ptr<class popup> active_popup;
        std::vector<window *> children;
        window *parent;
        bool open_popup_requested;
        dialog_result last_popup_result;
        bounds cached_bounds;
    protected:
        explicit window( int window_flags = 0 );
        explicit window( window *parent, int window_flags = 0 );
    public:
        explicit window( const std::string &title, int window_flags = 0 );
        virtual ~window();
        void draw_colored_text( std::string const &text, const nc_color &color,
                                text_align alignment = text_align::Left, float max_width = 0.0F, bool *is_selected = nullptr,
                                bool *is_focused = nullptr, bool *is_hovered = nullptr );
        void draw_colored_text( std::string const &text, nc_color &color,
                                text_align alignment = text_align::Left, float max_width = 0.0F, bool *is_selected = nullptr,
                                bool *is_focused = nullptr, bool *is_hovered = nullptr );
        bool action_button( const std::string &action, const std::string &text );
        void draw_header( std::string const &text );
        bool get_is_open() const;
        void set_title( const std::string &title );
        bool is_child_window_navigated();
        void show_popup_async( popup *next_popup );
        dialog_result show_popup( popup *next_popup );
        void show_popup_async( const std::shared_ptr<popup> &next_popup );
        dialog_result show_popup( const std::shared_ptr<popup> &next_popup );
        virtual void draw();
        bool is_resized();

    protected:
        bool is_open;
        std::string id;
        int window_flags;
        virtual bounds get_bounds();
        virtual void draw_controls() = 0;
        int draw_item_info_data( item_info_data &data );

        void add_child( window *child );
};

#if !(defined(TILES) || defined(WIN32))
void init_pair( int p, int f, int b );
void load_colors();
#endif
bool is_drag_drop_active();

class popup : public window
{
        friend class window;
        class popup_impl *p_impl;
        bool is_modal;
        std::function<bool()> on_draw_callback;
    public:
        popup( const std::string &id, bool is_modal );
        popup( const std::string &id, bool is_modal, const std::function<bool()> &on_draw_callback );
        ~popup() override;

        void draw() override;
        void set_draw_callback( const std::function<bool()> &callback );
        void close();
        dialog_result get_result();
        bool is_draw_callback_set();

    protected:
        dialog_result result;
        void open();
};
class message_box : public popup
{
        mbox_btn buttons;
        std::string prompt;
    public:
        message_box( const std::string &title, const std::string &prompt,
                     mbox_btn buttons = mbox_btn::BT_OK );
        static dialog_result show( const std::string &title, const std::string &text );
    protected:
        void draw_mbox_btn( const std::string &text, dialog_result result_if_clicked );
        void draw_controls() override;
};

class string_input_box : public popup
{
        std::string prompt;
        std::array<char, 100> input;
    public:
        string_input_box( const std::string &title, const std::string &prompt );
        static dialog_result show( const std::string &prompt, std::string &input );
        std::string get_input();
    protected:
        void draw_controls() override;
};

class list_selector : public popup
{
        std::string prompt;
        int selected_index;
    public:
        struct litem {
            std::string text;
            bool is_enabled;
            bool is_selected;
        };

        explicit list_selector( const std::string &id );
        void add( const litem &it );
        void add( std::initializer_list<litem> &items );
        int get_selected_index() const;
    protected:
        void draw_controls() override;
        std::vector<litem> items;
};
} // namespace cataimgui


