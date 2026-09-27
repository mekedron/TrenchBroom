/*
 Copyright (C) 2026 Nikita Rabykin

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ui/McpCompileHost.h"

#include <QObject>
#include <QTextEdit>

#include "base/NotifierConnection.h"
#include "gl/PerspectiveCamera.h"
#include "mdl/CompilationProfile.h"
#include "ui/CompilationRun.h"
#include "ui/MapDocument.h"
#include "ui/TextOutputAdapter.h"

#include <vector>

namespace tb::ui
{
namespace
{

class McpCompileJob : public mcp::CompileJob
{
private:
  MapDocument& m_document;
  mcp::CompileJobCallbacks m_callbacks;
  std::unique_ptr<gl::PerspectiveCamera> m_camera;
  std::unique_ptr<QTextEdit> m_output;
  std::unique_ptr<CompilationRun> m_run;
  std::vector<QMetaObject::Connection> m_connections;
  NotifierConnection m_notifierConnection;
  bool m_ended = false;
  /** Set while the job terminates the run itself and signals the end afterwards. */
  bool m_deferEnd = false;

public:
  McpCompileJob(
    MapDocument& document,
    mcp::CompileJobCallbacks callbacks,
    std::unique_ptr<gl::PerspectiveCamera> camera)
    : m_document{document}
    , m_callbacks{std::move(callbacks)}
    , m_camera{std::move(camera)}
    , m_output{std::make_unique<QTextEdit>()}
    , m_run{std::make_unique<CompilationRun>()}
  {
    m_output->setReadOnly(true);

    m_connections.push_back(
      QObject::connect(m_output.get(), &QTextEdit::textChanged, m_output.get(), [&]() {
        if (m_callbacks.outputChanged)
        {
          m_callbacks.outputChanged();
        }
      }));
    m_connections.push_back(QObject::connect(
      m_run.get(), &CompilationRun::compilationEnded, m_output.get(), [&]() {
        if (!m_deferEnd)
        {
          end();
        }
      }));

    m_notifierConnection += m_document.documentWasLoadedNotifier.connect(
      this, &McpCompileJob::documentWasLoaded);
  }

  ~McpCompileJob() override
  {
    m_notifierConnection.disconnect();
    for (const auto& connection : m_connections)
    {
      QObject::disconnect(connection);
    }
    m_run->terminate();
  }

  Result<void> start(const mdl::CompilationProfile& profile, const bool test)
  {
    const auto& map = m_document.map();
    return (test ? m_run->test(profile, map, *m_camera, m_output.get())
                 : m_run->run(profile, map, *m_camera, m_output.get()))
           | kdl::transform([&]() {
               if (!m_run->running())
               {
                 // the run ended synchronously or did not start any task
                 end();
               }
             });
  }

  std::string log() const override { return m_output->toPlainText().toStdString(); }

  bool running() const override { return !m_ended; }

  void cancel() override { m_run->terminate(); }

private:
  void end()
  {
    if (!m_ended)
    {
      m_ended = true;
      if (m_callbacks.ended)
      {
        m_callbacks.ended();
      }
    }
  }

  void documentWasLoaded()
  {
    if (m_ended)
    {
      return;
    }

    m_deferEnd = true;
    m_run->terminate();
    m_deferEnd = false;

    auto output = TextOutputAdapter{m_output.get()};
    output << "#### Terminated: the document was reloaded\n";
    end();
  }
};

} // namespace

std::unique_ptr<gl::PerspectiveCamera> copyPerspectiveCamera(
  const gl::PerspectiveCamera& camera)
{
  return std::make_unique<gl::PerspectiveCamera>(
    camera.fov(),
    camera.nearPlane(),
    camera.farPlane(),
    camera.viewport(),
    camera.position(),
    camera.direction(),
    camera.up());
}

McpCompileHost::McpCompileHost(CameraProvider cameraProvider)
  : m_cameraProvider{std::move(cameraProvider)}
{
}

McpCompileHost::~McpCompileHost() = default;

Result<std::unique_ptr<mcp::CompileJob>> McpCompileHost::startCompile(
  MapDocument& document,
  const mdl::CompilationProfile& profile,
  const bool test,
  mcp::CompileJobCallbacks callbacks)
{
  auto camera = m_cameraProvider ? m_cameraProvider(document) : nullptr;
  if (!camera)
  {
    camera = std::make_unique<gl::PerspectiveCamera>();
  }

  auto job =
    std::make_unique<McpCompileJob>(document, std::move(callbacks), std::move(camera));
  return job->start(profile, test) | kdl::transform([&]() {
           return std::unique_ptr<mcp::CompileJob>{std::move(job)};
         });
}

} // namespace tb::ui
