/*  This file is part of YUView - The YUV player with advanced analytics toolset
*   <https://github.com/IENT/YUView>
*   Copyright (C) 2015  Institut für Nachrichtentechnik, RWTH Aachen University, GERMANY
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 3 of the License, or
*   (at your option) any later version.
*
*   In addition, as a special exception, the copyright holders give
*   permission to link the code of portions of this program with the
*   OpenSSL library under certain conditions as described in each
*   individual source file, and distribute linked combinations including
*   the two.
*
*   You must obey the GNU General Public License in all respects for all
*   of the code used other than OpenSSL. If you modify file(s) with this
*   exception, you may extend this exception to your version of the
*   file(s), but you are not obligated to do so. If you do not wish to do
*   so, delete this exception statement from your version. If you delete
*   this exception statement from all source files in the program, then
*   also delete it here.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "playlistItemHEVCNextGenFile.h"

#include <QInputDialog>
#include <QPainter>
#include <QtConcurrent>
#include <QUrl>
#include "hevcNextGenDecoderJEM.h"
#include "hevcDecoderLibde265.h"
#include "signalsSlots.h"

#define HEVC_DEBUG_OUTPUT 0
#if HEVC_DEBUG_OUTPUT && !NDEBUG
#include <QDebug>
#define DEBUG_HEVC qDebug
#else
#define DEBUG_HEVC(fmt,...) ((void)0)
#endif

playlistItemHEVCNextGenFile::playlistItemHEVCNextGenFile(const QString &hevcFilePath, int displayComponent)
  : playlistItemWithVideo(hevcFilePath, playlistItem_Indexed)
{
  // Set the properties of the playlistItem
  setIcon(0, convertIcon(":img_videoHEVC.png"));
  setFlags(flags() | Qt::ItemIsDropEnabled);

  // Set the video pointer correctly
  video.reset(new videoHandlerYUV());
  videoHandlerYUV *yuvVideo = dynamic_cast<videoHandlerYUV*>(video.data());

  // Connect the basic signals from the video
  playlistItemWithVideo::connectVideo();

  // Nothing is currently being loaded
  isFrameLoading = false;
  isFrameLoadingDoubleBuffer = false;

  // An HEVC file can be cached if nothing goes wrong
  cachingEnabled = true;

  // Set which signal to show
  displaySignal = displayComponent;
  if (displaySignal < 0)
    displaySignal = 0;

  // Allocate the decoders
  loadingDecoder.reset(new hevcNextGenDecoderJEM(displaySignal));
  cachingDecoder.reset(new hevcNextGenDecoderJEM(displaySignal, true));
  
  // Reset display signal if this is not supported by the decoder
  if (displaySignal > loadingDecoder->wrapperNrSignalsSupported())
    displaySignal = 0;
  yuvVideo->showPixelValuesAsDiff = (displaySignal == 2 || displaySignal == 3);

  // Open the input file.
  if (!loadingDecoder->openFile(hevcFilePath))
  {
    // Something went wrong. Let's find out what.
    if (loadingDecoder->errorInDecoder())
      fileState = hevcFileOnlyParsing;
    if (loadingDecoder->errorParsingBitstream())
      fileState = hevcFileError;

    // In any case, decoding of images is not possible.
    cachingEnabled = false;
    return;
  }

  // The bitstream looks valid and the decoder is operational.
  fileState = hevcFileNoError;

  if (!cachingDecoder->openFile(hevcFilePath, loadingDecoder.data()))
  {
    // Loading the normal decoder worked, but loading another decoder for caching failed.
    // That is strange.
    cachingEnabled = false;
  }

  // Fill the list of statistics that we can provide
  fillStatisticList();

  // Set the frame number limits
  startEndFrame = getStartEndFrameLimits();

  if (startEndFrame.second == -1)
    // No frames to decode
    return;

  // Load frame 0. This will decode the first frame in the sequence and set the
  // correct frame size/YUV format.
  loadYUVData(0, false);

  // If the yuvVideHandler requests raw YUV data, we provide it from the file
  connect(yuvVideo, &videoHandlerYUV::signalRequestRawData, this, &playlistItemHEVCNextGenFile::loadYUVData, Qt::DirectConnection);
  connect(yuvVideo, &videoHandlerYUV::signalUpdateFrameLimits, this, &playlistItemHEVCNextGenFile::slotUpdateFrameLimits);
  connect(&statSource, &statisticHandler::updateItem, this, &playlistItemHEVCNextGenFile::updateStatSource);
  connect(&statSource, &statisticHandler::requestStatisticsLoading, this, &playlistItemHEVCNextGenFile::loadStatisticToCache);
}

void playlistItemHEVCNextGenFile::savePlaylist(QDomElement &root, const QDir &playlistDir) const
{
  // Determine the relative path to the HEVC file. We save both in the playlist.
  QUrl fileURL(plItemNameOrFileName);
  fileURL.setScheme("file");
  QString relativePath = playlistDir.relativeFilePath(plItemNameOrFileName);

  QDomElementYUView d = root.ownerDocument().createElement("playlistItemHEVCNextGenFile");

  // Append the properties of the playlistItem
  playlistItem::appendPropertiesToPlaylist(d);

  // Append all the properties of the HEVC file (the path to the file. Relative and absolute)
  d.appendProperiteChild("absolutePath", fileURL.toString());
  d.appendProperiteChild("relativePath", relativePath);
  d.appendProperiteChild("displayComponent", QString::number(displaySignal));

  root.appendChild(d);
}

playlistItemHEVCNextGenFile *playlistItemHEVCNextGenFile::newplaylistItemHEVCNextGenFile(const QDomElementYUView &root, const QString &playlistFilePath)
{
  // Parse the DOM element. It should have all values of a playlistItemHEVCNextGenFile
  QString absolutePath = root.findChildValue("absolutePath");
  QString relativePath = root.findChildValue("relativePath");
  int displaySignal = root.findChildValue("displayComponent").toInt();

  // check if file with absolute path exists, otherwise check relative path
  QString filePath = fileSource::getAbsPathFromAbsAndRel(playlistFilePath, absolutePath, relativePath);
  if (filePath.isEmpty())
    return nullptr;

  // We can still not be sure that the file really exists, but we gave our best to try to find it.
  playlistItemHEVCNextGenFile *newFile = new playlistItemHEVCNextGenFile(filePath, displaySignal);

  // Load the propertied of the playlistItemIndexed
  playlistItem::loadPropertiesFromPlaylist(root, newFile);

  return newFile;
}

infoData playlistItemHEVCNextGenFile::getInfo() const
{
  infoData info("HEVC File Info");

  // At first append the file information part (path, date created, file size...)
  info.items.append(loadingDecoder->getFileInfoList());

  if (fileState != hevcFileNoError)
    info.items.append(infoItem("Error", loadingDecoder->decoderErrorString()));
  if (fileState == hevcFileOnlyParsing)
  {
    info.items.append(infoItem("Num POCs", QString::number(loadingDecoder->getNumberPOCs()), "The number of pictures in the stream."));
    info.items.append(infoItem("NAL units", "Show NAL units", "Show a detailed list of all NAL units.", true));
  }
  else if (fileState == hevcFileNoError)
  {
    QSize videoSize = video->getFrameSize();
    info.items.append(infoItem("Decoder", loadingDecoder->getDecoderName()));
    info.items.append(infoItem("library path", loadingDecoder->getLibraryPath(), "The path to the loaded libde265 library"));
    info.items.append(infoItem("Resolution", QString("%1x%2").arg(videoSize.width()).arg(videoSize.height()), "The video resolution in pixel (width x height)"));
    info.items.append(infoItem("Num POCs", QString::number(loadingDecoder->getNumberPOCs()), "The number of pictures in the stream."));
    info.items.append(infoItem("Internals", loadingDecoder->wrapperInternalsSupported() ? "Yes" : "No", "Is the decoder able to provide internals (statistics)?"));
    info.items.append(infoItem("Stat Parsing", loadingDecoder->statisticsEnabled() ? "Yes" : "No", "Are the statistics of the sequence currently extracted from the stream?"));
    info.items.append(infoItem("NAL units", "Show NAL units", "Show a detailed list of all NAL units.", true));
  }

  return info;
}

void playlistItemHEVCNextGenFile::infoListButtonPressed(int buttonID)
{
  Q_UNUSED(buttonID);

  // Parse the annex B file again and save all the values read
  fileSourceHEVCAnnexBFile file;
  if (!file.openFile(plItemNameOrFileName, true))
    // Opening the file failed.
    return;

  // The button "Show NAL units" was pressed. Create a dialog with a QTreeView and show the NAL unit list.
  QDialog newDialog;
  QTreeView *view = new QTreeView();
  view->setModel(file.getNALUnitModel());
  QVBoxLayout *verticalLayout = new QVBoxLayout(&newDialog);
  verticalLayout->addWidget(view);
  newDialog.resize(QSize(700, 700));
  view->setColumnWidth(0, 400);
  view->setColumnWidth(1, 50);
  newDialog.exec();
}

itemLoadingState playlistItemHEVCNextGenFile::needsLoading(int frameIdx, bool loadRawData)
{
  auto videoState = video->needsLoading(frameIdx, loadRawData);
  if (videoState == LoadingNeeded || statSource.needsLoading(frameIdx) == LoadingNeeded)
    return LoadingNeeded;
  return videoState;
}

void playlistItemHEVCNextGenFile::drawItem(QPainter *painter, int frameIdx, double zoomFactor, bool drawRawData)
{
  if (fileState == hevcFileNoError && frameIdx >= 0 && frameIdx < loadingDecoder->getNumberPOCs())
  {
    video->drawFrame(painter, frameIdx, zoomFactor, drawRawData);
    statSource.paintStatistics(painter, frameIdx, zoomFactor);
  }
}

void playlistItemHEVCNextGenFile::loadYUVData(int frameIdx, bool caching)
{
  if (caching && !cachingEnabled)
    return;

  if (!caching && fileState != hevcFileNoError)
    // We can not decode images
    return;

  DEBUG_HEVC("playlistItemHEVCNextGenFile::loadYUVData %d %s", frameIdx, caching ? "caching" : "");

  videoHandlerYUV *yuvVideo = dynamic_cast<videoHandlerYUV*>(video.data());
  yuvVideo->setFrameSize(loadingDecoder->getFrameSize());
  yuvVideo->setYUVPixelFormat(loadingDecoder->getYUVPixelFormat());
  statSource.statFrameSize = loadingDecoder->getFrameSize();

  if (frameIdx > startEndFrame.second || frameIdx < 0)
  {
    DEBUG_HEVC("playlistItemHEVCNextGenFile::loadYUVData Invalid frame index");
    return;
  }

  // Just get the frame from the correct decoder
  QByteArray decByteArray;
  if (caching)
    decByteArray = cachingDecoder->loadYUVFrameData(frameIdx);
  else
    decByteArray = loadingDecoder->loadYUVFrameData(frameIdx);

  if (!decByteArray.isEmpty())
  {
    yuvVideo->rawYUVData = decByteArray;
    yuvVideo->rawYUVData_frameIdx = frameIdx;
  }
}

void playlistItemHEVCNextGenFile::createPropertiesWidget()
{
  // Absolutely always only call this once
  Q_ASSERT_X(!propertiesWidget, "playlistItemHEVCNextGenFile::createPropertiesWidget", "Always create the properties only once!");

  // Create a new widget and populate it with controls
  propertiesWidget.reset(new QWidget);
  ui.setupUi(propertiesWidget.data());

  QFrame *lineOne = new QFrame;
  lineOne->setObjectName(QStringLiteral("line"));
  lineOne->setFrameShape(QFrame::HLine);
  lineOne->setFrameShadow(QFrame::Sunken);
  QFrame *lineTwo = new QFrame;
  lineTwo->setObjectName(QStringLiteral("line"));
  lineTwo->setFrameShape(QFrame::HLine);
  lineTwo->setFrameShadow(QFrame::Sunken);

  // Insert a stretch at the bottom of the vertical global layout so that everything
  // gets 'pushed' to the top
  videoHandlerYUV *yuvVideo = dynamic_cast<videoHandlerYUV*>(video.data());
  ui.verticalLayout->insertLayout(0, createPlaylistItemControls());
  ui.verticalLayout->insertWidget(1, lineOne);
  ui.verticalLayout->insertLayout(2, yuvVideo->createYUVVideoHandlerControls(true));
  ui.verticalLayout->insertWidget(5, lineTwo);
  ui.verticalLayout->insertLayout(6, statSource.createStatisticsHandlerControls(), 1);

  // Set the components that we can display
  ui.comboBoxDisplaySignal->addItems(loadingDecoder->wrapperGetSignalNames());
  ui.comboBoxDisplaySignal->setCurrentIndex(displaySignal);

  // Connect signals/slots
  connect(ui.comboBoxDisplaySignal, QComboBox_currentIndexChanged_int, this, &playlistItemHEVCNextGenFile::displaySignalComboBoxChanged);
}

void playlistItemHEVCNextGenFile::fillStatisticList()
{
  if (!loadingDecoder->wrapperInternalsSupported())
    return;

  loadingDecoder->fillStatisticList(statSource);
}

void playlistItemHEVCNextGenFile::loadStatisticToCache(int frameIdx, int typeIdx)
{
  Q_UNUSED(typeIdx);
  DEBUG_HEVC("playlistItemHEVCNextGenFile::loadStatisticToCache Request statistics type %d for frame %d", typeIdx, frameIdx);

  if (!loadingDecoder->wrapperInternalsSupported())
    return;

  statSource.statsCache[typeIdx] = loadingDecoder->getStatisticsData(frameIdx, typeIdx);
}

ValuePairListSets playlistItemHEVCNextGenFile::getPixelValues(const QPoint &pixelPos, int frameIdx)
{
  ValuePairListSets newSet;

  newSet.append("YUV", video->getPixelValues(pixelPos, frameIdx));
  if (loadingDecoder->wrapperInternalsSupported() && loadingDecoder->statisticsEnabled())
    newSet.append("Stats", statSource.getValuesAt(pixelPos));

  return newSet;
}

void playlistItemHEVCNextGenFile::getSupportedFileExtensions(QStringList &allExtensions, QStringList &filters)
{
  allExtensions.append("hevc");
  allExtensions.append("bin");
  filters.append("Annex B HEVC Bitstream (*.hevc, *.bin)");
}

void playlistItemHEVCNextGenFile::reloadItemSource()
{
  // TODO: The caching decoder must also be reloaded
  //       All items in the cache are also now invalid

  loadingDecoder->reloadItemSource();

  // Set the frame number limits
  startEndFrame = getStartEndFrameLimits();

  // Reset the videoHandlerYUV source. With the next draw event, the videoHandlerYUV will request to decode the frame again.
  video->invalidateAllBuffers();

  // Load frame 0. This will decode the first frame in the sequence and set the
  // correct frame size/YUV format.
  loadYUVData(0, false);
}

void playlistItemHEVCNextGenFile::cacheFrame(int idx, bool testMode)
{
  if (!cachingEnabled)
    return;

  // Cache a certain frame. This is always called in a separate thread.
  cachingMutex.lock();
  video->cacheFrame(idx, testMode);
  cachingMutex.unlock();
}

void playlistItemHEVCNextGenFile::loadFrame(int frameIdx, bool playing, bool loadRawdata, bool emitSignals)
{
  auto stateYUV = video->needsLoading(frameIdx, loadRawdata);
  auto stateStat = statSource.needsLoading(frameIdx);

  if (stateYUV == LoadingNeeded || stateStat == LoadingNeeded)
  {
    isFrameLoading = true;
    if (stateYUV == LoadingNeeded)
    {
      // Load the requested current frame
      DEBUG_HEVC("playlistItemRawFile::loadFrame loading frame %d %s", frameIdx, playing ? "(playing)" : "");
      video->loadFrame(frameIdx);
    }
    if (stateStat == LoadingNeeded)
    {
      DEBUG_HEVC("playlistItemRawFile::loadFrame loading statistics %d %s", frameIdx, playing ? "(playing)" : "");
      statSource.loadStatistics(frameIdx);
    }

    isFrameLoading = false;
    if (emitSignals)
      emit signalItemChanged(true, false);
  }

  if (playing && (stateYUV == LoadingNeeded || stateYUV == LoadingNeededDoubleBuffer))
  {
    // Load the next frame into the double buffer
    int nextFrameIdx = frameIdx + 1;
    if (nextFrameIdx <= startEndFrame.second)
    {
      DEBUG_HEVC("playlistItemRawFile::loadFrame loading frame into double buffer %d %s", nextFrameIdx, playing ? "(playing)" : "");
      isFrameLoadingDoubleBuffer = true;
      video->loadFrame(nextFrameIdx, true);
      isFrameLoadingDoubleBuffer = false;
      if (emitSignals)
        emit signalItemDoubleBufferLoaded();
    }
  }
}

void playlistItemHEVCNextGenFile::displaySignalComboBoxChanged(int idx)
{
  if (displaySignal != idx)
  {
    displaySignal = idx;
    loadingDecoder->setDecodeSignal(idx);
    cachingDecoder->setDecodeSignal(idx);

    // A different display signal was chosen. Invalidate the cache and signal that we will need a redraw.
    videoHandlerYUV *yuvVideo = dynamic_cast<videoHandlerYUV*>(video.data());
    yuvVideo->showPixelValuesAsDiff = (idx == 2 || idx == 3);
    yuvVideo->invalidateAllBuffers();
    emit signalItemChanged(true, true);
  }
}
