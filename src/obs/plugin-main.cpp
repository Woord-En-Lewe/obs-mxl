// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include <obs-frontend-api.h>
#include <obs-module.h>

#if defined(OBSMXL_HAVE_UI)
#include <QAction>
#include <QMainWindow>
#include <QObject>
#endif

#include <obsmxl/version.hpp>

#include "mxl-output.hpp"
#include "mxl-source.hpp"
#include "obs-log.hpp"
#include "output-manager.hpp"

#if defined(OBSMXL_HAVE_UI)
#include "ui/mxl-output-dialog.hpp"
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-mxl", "en-US")

namespace
{
#if defined(OBSMXL_HAVE_UI)
obsmxl::obs::MxlOutputDialog* output_dialog = nullptr;
#endif

void frontend_event(enum obs_frontend_event event, void*)
{
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
    {
        obsmxl::obs::OutputManager::instance().auto_start();
    }
    else if (event == OBS_FRONTEND_EVENT_EXIT)
    {
        obsmxl::obs::OutputManager::instance().stop_all();
    }
}

#if defined(OBSMXL_HAVE_UI)
void show_output_dialog()
{
    if (!output_dialog)
    {
        auto* main_window = static_cast<QMainWindow*>(obs_frontend_get_main_window());
        output_dialog = new obsmxl::obs::MxlOutputDialog(main_window);
    }
    output_dialog->show();
    output_dialog->raise();
    output_dialog->activateWindow();
}
#endif
} // namespace

MODULE_EXPORT const char* obs_module_description(void)
{
    return "EBU MXL uncompressed ingress (sources) and egress (outputs)";
}

bool obs_module_load(void)
{
    blog(LOG_INFO, "[obs-mxl] loading unified MXL plugin %s", obsmxl::version_string);

    obsmxl::obs::register_mxl_source();
    obsmxl::obs::register_mxl_output();

#if defined(OBSMXL_HAVE_UI)
    if (auto* action = static_cast<QAction*>(
            obs_frontend_add_tools_menu_qaction(obs_module_text("Tools.MxlOutputSettings"))))
    {
        QObject::connect(action, &QAction::triggered, &show_output_dialog);
    }
#endif

    obs_frontend_add_event_callback(&frontend_event, nullptr);
    return true;
}

void obs_module_unload(void)
{
    obs_frontend_remove_event_callback(&frontend_event, nullptr);
    obsmxl::obs::OutputManager::instance().stop_all();

#if defined(OBSMXL_HAVE_UI)
    delete output_dialog;
    output_dialog = nullptr;
#endif

    blog(LOG_INFO, "[obs-mxl] plugin unloaded");
}
