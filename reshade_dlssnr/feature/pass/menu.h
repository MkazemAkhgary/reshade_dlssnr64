// Context-menu commands that need to know what a pass order is.
//
// "Previous" and "next" are positions in the tab order, which only this
// feature knows. The commands are registered once and serve every target the
// menu can open on -- a row, a section header, or the pass tab -- because the
// target already carries the instance key they resolve against.

#pragma once

namespace reshade_dlssnr {
namespace ctl {
class MenuCommandRegistry;
}  // namespace ctl

namespace feature {
namespace pass {

void RegisterPassMenuCommands(ctl::MenuCommandRegistry& registry);

}  // namespace pass
}  // namespace feature
}  // namespace reshade_dlssnr
