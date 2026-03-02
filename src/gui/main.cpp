#include "gui/gui_controller.hpp"

int main() {
    kirdi::gui::GuiController controller;
    controller.run("gui/dist/index.html");
    return 0;
}
