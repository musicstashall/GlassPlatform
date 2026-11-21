#pragma once
#include <MenuEventProvider.h>
#include <DropDownEventProvider.h>
#include <RootEventProvider.h>
#include <DirectUIEventProvider.h>
#include <RebarEventProvider.h>
#include <CoolBarEventProvider.h>
#include <TooltipEventProvider.h>

struct GlassCoreProviders
{
    WndCompat::Menu::MenuEventProvider menu;
    WndCompat::DropDown::DropDownEventProvider dropdown;
    WndCompat::Root::RootEventProvider root;
    WndCompat::DirectUI::DirectUIEventProvider directui;
    WndCompat::Rebar::RebarEventProvider rebar;
    WndCompat::CoolBar::CoolBarEventProvider coolbar;
    WndCompat::Tooltip::TooltipEventProvider tooltip;
};
