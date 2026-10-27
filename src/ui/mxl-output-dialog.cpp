// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

#include "mxl-output-dialog.hpp"

#include <QFileDialog>
#include <QTimer>

#include <obs-module.h>

#include <obsmxl/uuid.hpp>

#include "obs/obs-config-store.hpp"
#include "obs/output-manager.hpp"
#include "ui_mxl-output-dialog.h"

namespace obsmxl::obs
{
MxlOutputDialog::MxlOutputDialog(QWidget* parent)
    : QDialog(parent), ui_(std::make_unique<Ui::MxlOutputDialog>())
{
    ui_->setupUi(this);
    setWindowTitle(QString::fromUtf8(obs_module_text("Dialog.Title")));
    ui_->errorLabel->setStyleSheet("color: #e05a5a;");

    connect(ui_->applyButton, &QPushButton::clicked, this, &MxlOutputDialog::onApply);
    connect(ui_->closeButton, &QPushButton::clicked, this, &MxlOutputDialog::hide);
    connect(ui_->browseDomainButton, &QPushButton::clicked, this, &MxlOutputDialog::onBrowseDomain);
    connect(ui_->programGenerateVideo, &QPushButton::clicked, this,
            &MxlOutputDialog::onGenerateProgramVideo);
    connect(ui_->programGenerateAudio, &QPushButton::clicked, this,
            &MxlOutputDialog::onGenerateProgramAudio);
    connect(ui_->previewGenerateVideo, &QPushButton::clicked, this,
            &MxlOutputDialog::onGeneratePreviewVideo);
    connect(ui_->previewGenerateAudio, &QPushButton::clicked, this,
            &MxlOutputDialog::onGeneratePreviewAudio);

    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &MxlOutputDialog::updateStatus);
    timer->start(1000);

    load();
    updateStatus();
}

MxlOutputDialog::~MxlOutputDialog() = default;

void MxlOutputDialog::load()
{
    writeUi(load_settings());
}

void MxlOutputDialog::writeUi(PluginSettings const& settings)
{
    ui_->domainPath->setText(QString::fromStdString(settings.domain_path));
    ui_->historySpin->setValue(settings.history_ms);
    ui_->manageHistoryCheck->setChecked(settings.manage_domain_history);

    ui_->programEnabled->setChecked(settings.program.enabled);
    ui_->programFlowName->setText(QString::fromStdString(settings.program.flow_name));
    ui_->programVideoUuid->setText(QString::fromStdString(settings.program.video_flow_uuid));
    ui_->programAudioUuid->setText(QString::fromStdString(settings.program.audio_flow_uuid));
    ui_->programIncludeAudio->setChecked(settings.program.include_audio);

    ui_->previewEnabled->setChecked(settings.preview.enabled);
    ui_->previewFlowName->setText(QString::fromStdString(settings.preview.flow_name));
    ui_->previewVideoUuid->setText(QString::fromStdString(settings.preview.video_flow_uuid));
    ui_->previewAudioUuid->setText(QString::fromStdString(settings.preview.audio_flow_uuid));
    ui_->previewIncludeAudio->setChecked(settings.preview.include_audio);
}

PluginSettings MxlOutputDialog::readUi() const
{
    PluginSettings settings;
    settings.domain_path = ui_->domainPath->text().toStdString();
    settings.history_ms = ui_->historySpin->value();
    settings.manage_domain_history = ui_->manageHistoryCheck->isChecked();

    settings.program.enabled = ui_->programEnabled->isChecked();
    settings.program.flow_name = ui_->programFlowName->text().toStdString();
    settings.program.video_flow_uuid = ui_->programVideoUuid->text().trimmed().toStdString();
    settings.program.audio_flow_uuid = ui_->programAudioUuid->text().trimmed().toStdString();
    settings.program.include_audio = ui_->programIncludeAudio->isChecked();

    settings.preview.enabled = ui_->previewEnabled->isChecked();
    settings.preview.flow_name = ui_->previewFlowName->text().toStdString();
    settings.preview.video_flow_uuid = ui_->previewVideoUuid->text().trimmed().toStdString();
    settings.preview.audio_flow_uuid = ui_->previewAudioUuid->text().trimmed().toStdString();
    settings.preview.include_audio = ui_->previewIncludeAudio->isChecked();

    return settings;
}

void MxlOutputDialog::onApply()
{
    auto settings = readUi();

    // Keep the audio flow UUID derived from the video UUID unless the user
    // intentionally typed a different one.
    auto ensure_audio = [](OutputEndpointSettings& endpoint)
    {
        if (endpoint.include_audio && !is_valid_uuid(endpoint.audio_flow_uuid))
        {
            if (auto derived = derive_uuid(endpoint.video_flow_uuid, "/audio"))
            {
                endpoint.audio_flow_uuid = format_uuid(*derived);
            }
        }
    };
    ensure_audio(settings.program);
    ensure_audio(settings.preview);
    writeUi(settings);

    std::string error;
    if (!OutputManager::instance().apply(settings, &error))
    {
        ui_->errorLabel->setText(QString::fromStdString(error));
    }
    else
    {
        ui_->errorLabel->clear();
    }
    updateStatus();
}

void MxlOutputDialog::onBrowseDomain()
{
    auto const directory = QFileDialog::getExistingDirectory(
        this, tr("Select MXL shared memory domain"), ui_->domainPath->text());
    if (!directory.isEmpty())
    {
        ui_->domainPath->setText(directory);
    }
}

void MxlOutputDialog::onGenerateProgramVideo()
{
    auto const video = format_uuid(generate_uuid_v4());
    ui_->programVideoUuid->setText(QString::fromStdString(video));
    if (auto derived = derive_uuid(video, "/audio"))
    {
        ui_->programAudioUuid->setText(QString::fromStdString(format_uuid(*derived)));
    }
}

void MxlOutputDialog::onGenerateProgramAudio()
{
    ui_->programAudioUuid->setText(QString::fromStdString(format_uuid(generate_uuid_v4())));
}

void MxlOutputDialog::onGeneratePreviewVideo()
{
    auto const video = format_uuid(generate_uuid_v4());
    ui_->previewVideoUuid->setText(QString::fromStdString(video));
    if (auto derived = derive_uuid(video, "/audio"))
    {
        ui_->previewAudioUuid->setText(QString::fromStdString(format_uuid(*derived)));
    }
}

void MxlOutputDialog::onGeneratePreviewAudio()
{
    ui_->previewAudioUuid->setText(QString::fromStdString(format_uuid(generate_uuid_v4())));
}

void MxlOutputDialog::updateStatus()
{
    auto const& manager = OutputManager::instance();

    auto const text = [&manager](OutputRole role)
    {
        std::string status = manager.status(role);
        auto const bullet = (status.rfind("ACTIVE", 0) == 0)
                                ? QString(QChar(0x25CF)) + QStringLiteral(" ")
                                : QString(QChar(0x25CB)) + QStringLiteral(" ");
        return QStringLiteral("Status: ") + bullet + QString::fromStdString(status);
    };

    ui_->programStatus->setText(text(OutputRole::Program));
    ui_->previewStatus->setText(text(OutputRole::Preview));
}
} // namespace obsmxl::obs
