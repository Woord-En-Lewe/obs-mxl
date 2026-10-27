// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#pragma once

#include <memory>

#include <QDialog>

#include <obsmxl/settings.hpp>

namespace Ui
{
class MxlOutputDialog;
}

namespace obsmxl::obs
{
/**
 * Tools -> MXL Output Settings dialog. It is a thin view over
 * OutputManager: the outputs keep running when the dialog is closed.
 */
class MxlOutputDialog final : public QDialog
{
    Q_OBJECT

public:
    explicit MxlOutputDialog(QWidget* parent = nullptr);
    ~MxlOutputDialog() override;

private slots:
    void onApply();
    void onBrowseDomain();
    void onGenerateProgramVideo();
    void onGenerateProgramAudio();
    void onGeneratePreviewVideo();
    void onGeneratePreviewAudio();
    void updateStatus();

private:
    void load();
    [[nodiscard]] PluginSettings readUi() const;
    void writeUi(PluginSettings const& settings);

    std::unique_ptr<Ui::MxlOutputDialog> ui_;
};
} // namespace obsmxl::obs
