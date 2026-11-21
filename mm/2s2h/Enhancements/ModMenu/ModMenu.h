#pragma once

#include <libultraship/libultraship.h>
#include <string>

#ifdef __cplusplus
void ModMenu_LoadArchives();
bool ModMenu_StartGame(std::string& error);
void DrawSkinSelector();

class ModMenuWindow : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;

    void InitElement() override;
    void Draw() override;
    void DrawElement() override;
    void UpdateElement() override{};
};
#endif