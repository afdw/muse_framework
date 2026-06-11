/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "dockwindow.h"

#include <QTimer>

#include "kddockwidgets/src/LayoutSaver.h"
#include "kddockwidgets/src/Config.h"
#include "kddockwidgets/src/core/DockRegistry.h"
#include "kddockwidgets/src/core/Layout.h"
#include "kddockwidgets/src/core/MainWindow.h"
#include "kddockwidgets/src/qtquick/views/MainWindow.h"
#include "kddockwidgets/src/qtquick/views/DockWidget.h"
#include "kddockwidgets/src/qtquick/views/View.h"
#include "kddockwidgets/src/core/Group.h"
#include "kddockwidgets/src/core/Window_p.h"

#include "global/async/async.h"

#include "dockcentralview.h"
#include "dockpageview.h"
#include "dockpanelview.h"
#include "dockstatusbar.h"
#include "docktoolbarview.h"
#include "dockingholderview.h"
#include "dockwindow.h"

#include "muse_framework_config.h"

#include "log.h"

using namespace muse::dock;
using namespace muse::async;

namespace muse::dock {
//! NOTE: Toolbar's maximum lenght, see DockToolBar.qml
static constexpr int UNCONSTRAINED_TOOLBAR_WIDTH = 16777215;

static const QList<Location> POSSIBLE_LOCATIONS {
    Location::Left,
    Location::Right,
    Location::Top,
    Location::Bottom
};

static KDDockWidgets::Location locationToKLocation(Location location)
{
    switch (location) {
    case Location::Left: return KDDockWidgets::Location_OnLeft;
    case Location::Right: return KDDockWidgets::Location_OnRight;
    case Location::Top: return KDDockWidgets::Location_OnTop;
    case Location::Bottom: return KDDockWidgets::Location_OnBottom;
    case Location::Center: break;
    case Location::Undefined: break;
    }

    return KDDockWidgets::Location_None;
}

static void clearRegistry(int ctx)
{
    TRACEFUNC;

    auto* registry = KDDockWidgets::DockRegistry::self(ctx);

    for (KDDockWidgets::Core::MainWindow* mw : registry->mainwindows()) {
        mw->layout()->clearLayout();
    }

    registry->clear();

    for (KDDockWidgets::Core::DockWidget* dock : registry->dockwidgets()) {
        registry->unregisterDockWidget(dock);
    }

    for (KDDockWidgets::Core::Group* group : registry->groups()) {
        const auto dockWidgets = group->dockWidgets();
        for (KDDockWidgets::Core::DockWidget* dock : dockWidgets) {
            group->removeWidget(dock);
        }
        registry->unregisterGroup(group);
    }
}
}

class DockWindow::UniqueConnectionHolder : public QObject
{
    Q_OBJECT
public:
    UniqueConnectionHolder(DockPageView* page, DockWindow* parent)
        : QObject(parent), m_page(page) {}

    void alignTopLevelToolBars()
    {
        static_cast<DockWindow*>(parent())->alignTopLevelToolBars(m_page);
    }

private:
    DockPageView* m_page = nullptr;
};

DockWindow::DockWindow(QQuickItem* parent)
    : QQuickItem(parent), muse::Contextable(muse::iocCtxForQmlObject(this)),
    m_toolBars(this),
    m_pages(this)
{
}

DockWindow::~DockWindow()
{
    dockWindowProvider()->deinit();

    // Without this, the connections would be deleted by the QObject destructor,
    // because they are child objects of this. But since they use DockWindow-
    // specific code (rather than QObject-specific), we need to delete them
    // before the end of the DockWindow destructor.
    qDeleteAll(m_pageConnections);
}

void DockWindow::componentComplete()
{
    TRACEFUNC;

    QQuickItem::componentComplete();

    static const QString name = "mainWindow";

    m_mainWindow = new KDDockWidgets::QtQuick::MainWindow(iocContext()->id, name,
                                                          KDDockWidgets::MainWindowOption_None,
                                                          this);

    connect(qApp, &QCoreApplication::aboutToQuit, this, &DockWindow::onQuit);
    connect(this, &QQuickItem::windowChanged, this, &DockWindow::windowPropertyChanged);
}

void DockWindow::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry)
{
    const bool widthChanged = m_currentPage && !qFuzzyCompare(newGeometry.width(), oldGeometry.width());
    const bool shrinking = widthChanged && newGeometry.width() < oldGeometry.width();

    if (shrinking) {
        m_pendingWidth = int(newGeometry.width());

        //! NOTE: Unpin the paddings first so compact mode can actually shrink the toolbars' content
        for (DockToolBarView* toolBar : topLevelToolBars(m_currentPage)) {
            toolBar->setMinimumWidth(toolBar->contentWidth());
        }

        adjustContentForAvailableSpace(m_currentPage);
        alignTopLevelToolBars(m_currentPage);

        //! NOTE: Apply the new size to the layout synchronously
        applyLayoutSizeToFitWindow();

        m_pendingWidth = -1;

        //! NOTE: The synchronous resize above can be partial when KDDockWidgets' async item-size sync
        //! hasn't settled yet. So let's shedule another layout
        scheduleDeferredLayoutFix();
    }

    QQuickItem::geometryChange(newGeometry, oldGeometry);

    if (!widthChanged || shrinking) {
        return;
    }

    adjustContentForAvailableSpace(m_currentPage);
    alignTopLevelToolBars(m_currentPage);
}

void DockWindow::applyLayoutSizeToFitWindow()
{
    KDDockWidgets::Core::MainWindow* mw = m_mainWindow ? m_mainWindow->mainWindow() : nullptr;
    KDDockWidgets::Core::Layout* layout = mw ? mw->layout() : nullptr;
    if (!layout) {
        return;
    }

    const int targetWidth = m_pendingWidth >= 0 ? m_pendingWidth : int(width());
    const int layoutMin = layout->layoutMinimumSize().width();

    const int applyWidth = std::max(targetWidth, layoutMin);

    if (applyWidth != layout->layoutSize().width()) {
        layout->setLayoutSize(QSize(applyWidth, layout->layoutSize().height()));
    }
}

void DockWindow::scheduleDeferredLayoutFix()
{
    if (m_layoutFixScheduled) {
        return;
    }

    m_layoutFixScheduled = true;

    QTimer::singleShot(0, this, [this]() {
        runDeferredLayoutFix();
    });
}

void DockWindow::runDeferredLayoutFix()
{
    m_layoutFixScheduled = false;

    if (!m_currentPage) {
        return;
    }

    adjustContentForAvailableSpace(m_currentPage);
    alignTopLevelToolBars(m_currentPage);

    //! NOTE: Force the layouting items to pick up the current dock minimums
    for (DockBase* dock : m_currentPage->allDocks()) {
        if (dock) {
            dock->syncLayoutItemMinSize();
        }
    }

    applyLayoutSizeToFitWindow();
}

void DockWindow::onQuit()
{
    TRACEFUNC;

    IF_ASSERT_FAILED(m_currentPage) {
        return;
    }

    savePageState(m_currentPage->objectName());
    m_reloadCurrentPageAllowed = false;

    clearRegistry(iocContext()->id);

    saveWindowGeometry();
}

QString DockWindow::currentPageUri() const
{
    return m_currentPage ? m_currentPage->uri() : QString();
}

QQmlListProperty<muse::dock::DockToolBarView> DockWindow::toolBarsProperty()
{
    return m_toolBars.property();
}

QQmlListProperty<muse::dock::DockPageView> DockWindow::pagesProperty()
{
    return m_pages.property();
}

QQuickWindow* DockWindow::windowProperty() const
{
    return window();
}

void DockWindow::init()
{
    clearRegistry(iocContext()->id);
    restoreGeometry();

    dockWindowProvider()->init(this);

    uiState()->windowGeometryChanged().onNotify(this, [this]() {
        reloadCurrentPage();
    });

    workspaceManager()->currentWorkspaceAboutToBeChanged().onNotify(this, [this]() {
        if (const DockPageView* page = currentPage()) {
            savePageState(page->objectName());
        }
    });
}

void DockWindow::loadPage(const QString& uri, const QVariantMap& params)
{
    TRACEFUNC;

    if (currentPageUri() == uri) {
        if (m_currentPage) {
            m_currentPage->setParams(params);
        }
        return;
    }

    const bool isFirstOpening = (m_currentPage == nullptr);

    if (!isFirstOpening) {
        const QString pageName = m_currentPage->objectName();
        uiState()->pageState(pageName).notification.disconnect(this);
        savePageState(pageName);
        clearRegistry(iocContext()->id);
        m_currentPage->setVisible(false);
        m_currentPage->deinit();
    }

    bool ok = doLoadPage(uri, params);
    if (!ok) {
        return;
    }

    if (checkLayoutIsCorrupted()) {
        LOGE() << "Layout is corrupted, restoring default";
        restoreDefaultLayout();
    }

    auto notifyAboutPageLoaded = [this, &uri]() {
        emit currentPageUriChanged(uri);
        emit pageLoaded();
        notifyAboutDocksOpenStatus();
    };

    if (isFirstOpening) {
        async::Async::call(this, [this, notifyAboutPageLoaded]() {
            if (!m_hasGeometryBeenRestored
                || (m_mainWindow->window()->isFullScreen())) {
                //! NOTE: show window as maximized if no geometry has been restored
                //! or if the user had closed app in FullScreen mode
                // m_mainWindow->window()->->showMaximized(); // todo kddock
            }

            notifyAboutPageLoaded();
        });
    } else {
        notifyAboutPageLoaded();
    }
}

bool DockWindow::isDockOpen(const QString& dockName) const
{
    return m_currentPage && m_currentPage->isDockOpen(dockName);
}

void DockWindow::toggleDock(const QString& dockName)
{
    if (m_currentPage) {
        m_currentPage->toggleDock(dockName);
        m_docksOpenStatusChanged.send({ dockName });
    }
}

void DockWindow::setDockOpen(const QString& dockName, bool open)
{
    if (m_currentPage) {
        m_currentPage->setDockOpen(dockName, open);
        m_docksOpenStatusChanged.send({ dockName });
    }
}

Channel<QStringList> DockWindow::docksOpenStatusChanged() const
{
    return m_docksOpenStatusChanged;
}

bool DockWindow::isDockFloating(const QString& dockName) const
{
    return m_currentPage && m_currentPage->isDockFloating(dockName);
}

void DockWindow::toggleDockFloating(const QString& dockName)
{
    if (m_currentPage) {
        m_currentPage->toggleDockFloating(dockName);
    }
}

DockPageView* DockWindow::currentPage() const
{
    return m_currentPage;
}

QQuickItem& DockWindow::asItem() const
{
    return *m_mainWindow;
}

void DockWindow::restoreDefaultLayout()
{
    TRACEFUNC;

    //! HACK: notify about upcoming change of current URI
    //! so that all subscribers of this channel finish their work.
    //! For example, our popups and tooltips will close.
    interactive()->currentUriAboutToBeChanged().notify();

    if (m_currentPage) {
        for (DockBase* dock : m_currentPage->allDocks()) {
            dock->resetToDefault();
        }
    }

    m_reloadCurrentPageAllowed = false;
    for (const DockPageView* page : m_pages.list()) {
        uiState()->setPageState(page->objectName(), QByteArray());
    }

    uiState()->setWindowGeometry(QByteArray());
    m_reloadCurrentPageAllowed = true;

    reloadCurrentPage();
}

void DockWindow::loadPageContent(const DockPageView* page)
{
    TRACEFUNC;

    addDock(page->centralDock());

    loadPanels(page);
    loadToolBars(page);

    if (page->statusBar()) {
        addDock(page->statusBar(), Location::Bottom);
    }

    loadTopLevelToolBars(page);
}

void DockWindow::loadTopLevelToolBars(const DockPageView* page)
{
    TRACEFUNC;

    QList<DockToolBarView*> allToolBars = m_toolBars.list();
    allToolBars << page->mainToolBars();

    DockToolBarView* prevToolBar = nullptr;

    for (DockToolBarView* toolBar : allToolBars) {
        auto location = prevToolBar ? Location::Right : Location::Top;
        addDock(toolBar, location, prevToolBar);
        prevToolBar = toolBar;
    }
}

void DockWindow::loadToolBars(const DockPageView* page)
{
    TRACEFUNC;

    for (DockToolBarView* toolBar : page->toolBars()) {
        addDock(toolBar, toolBar->location());
    }

    for (Location location : POSSIBLE_LOCATIONS) {
        if (auto holder = page->holder(DockType::ToolBar, location)) {
            addDock(holder, location);
        }
    }
}

void DockWindow::loadPanels(const DockPageView* page)
{
    TRACEFUNC;

    for (DockPanelView* panel : page->panels()) {
        if (DockPanelView* destinationPanel = page->findPanelForTab(panel)) {
            addPanelAsTab(panel, destinationPanel);
            continue;
        }

        const Location location = panel->location();
        const bool isSideLocation = location == Location::Left || location == Location::Right;
        addDock(panel, location, isSideLocation ? page->centralDock() : nullptr);
    }

    for (Location location : POSSIBLE_LOCATIONS) {
        if (auto holder = page->holder(DockType::Panel, location)) {
            addDock(holder, location);
        }
    }
}

void DockWindow::alignTopLevelToolBars(const DockPageView* page)
{
    const QList<DockToolBarView*> topToolBars = topLevelToolBars(page);

    //! NOTE: Reset paddings/pins first, so that widths from a previous
    //! configuration do not accumulate
    for (DockToolBarView* toolBar : topToolBars) {
        toolBar->setMaximumWidth(UNCONSTRAINED_TOOLBAR_WIDTH);
        toolBar->setMinimumWidth(toolBar->contentWidth());
    }

    DockToolBarView* lastLeftToolBar = nullptr;
    DockToolBarView* lastCentralToolBar = nullptr;

    int leftBlockWidth = 0;
    int centralBlockWidth = 0;
    int rightBlockWidth = 0;

    int leftCount = 0;
    int centralCount = 0;
    int rightCount = 0;

    const int separator = KDDockWidgets::Config::self(iocContext()->id).separatorThickness();

    for (DockToolBarView* toolBar : topToolBars) {
        if (toolBar->floating() || !toolBar->isVisible()) {
            continue;
        }

        switch (static_cast<DockToolBarAlignment::Type>(toolBar->alignment())) {
        case DockToolBarAlignment::Left:
            lastLeftToolBar = toolBar;
            leftBlockWidth += toolBar->contentWidth();
            ++leftCount;
            break;
        case DockToolBarAlignment::Center:
            lastCentralToolBar = toolBar;
            centralBlockWidth += toolBar->contentWidth();
            ++centralCount;
            break;
        case DockToolBarAlignment::Right:
            rightBlockWidth += toolBar->contentWidth();
            ++rightCount;
            break;
        }
    }

    leftBlockWidth += separator * std::max(0, leftCount - 1);
    centralBlockWidth += separator * std::max(0, centralCount - 1);
    rightBlockWidth += separator * std::max(0, rightCount - 1);

    const int separatorLeftCentral = (leftCount > 0 && centralCount > 0) ? separator : 0;
    const int separatorCentralRight = (centralCount > 0 && rightCount > 0) ? separator : 0;
    const int separatorLeftRight = (leftCount > 0 && centralCount == 0 && rightCount > 0) ? separator : 0;

    const int occupied = leftBlockWidth + centralBlockWidth + rightBlockWidth
                         + separatorLeftCentral + separatorCentralRight + separatorLeftRight;

    //! NOTE: During a resize the new width isn't propagated to width yet
    const int availableWidth = m_pendingWidth >= 0 ? m_pendingWidth : int(width());

    const int freeSpace = availableWidth - occupied;
    if (freeSpace <= 0) {
        return;
    }

    DockToolBarView* grower = lastLeftToolBar ? lastLeftToolBar : lastCentralToolBar;

    int paddingForLastCentral = 0;
    if (lastCentralToolBar && lastCentralToolBar != grower) {
        paddingForLastCentral = (availableWidth - centralBlockWidth) / 2 - rightBlockWidth - separatorCentralRight;
        paddingForLastCentral = std::min(std::max(paddingForLastCentral, 0), freeSpace);
    }

    for (DockToolBarView* toolBar : topToolBars) {
        if (toolBar->floating() || !toolBar->isVisible() || toolBar == grower) {
            continue;
        }

        const int pinnedWidth = (toolBar == lastCentralToolBar)
                                ? toolBar->contentWidth() + paddingForLastCentral
                                : toolBar->contentWidth();

        //! NOTE: max before min
        toolBar->setMaximumWidth(pinnedWidth);
        toolBar->setMinimumWidth(pinnedWidth);
    }
}

void DockWindow::addDock(DockBase* dock, Location location, const DockBase* relativeTo)
{
    TRACEFUNC;

    registerDock(dock);

    auto* dockWidgetView = qobject_cast<KDDockWidgets::QtQuick::DockWidget*>(
        KDDockWidgets::QtQuick::asQQuickItem(dock->dockWidget()));

    KDDockWidgets::QtQuick::DockWidget* relativeDockWidgetView = nullptr;
    if (relativeTo) {
        relativeDockWidgetView = qobject_cast<KDDockWidgets::QtQuick::DockWidget*>(
            KDDockWidgets::QtQuick::asQQuickItem(relativeTo->dockWidget()));
    }

    auto visibilityOption = dock->defaultVisibility() ? KDDockWidgets::InitialVisibilityOption::StartVisible
                            : KDDockWidgets::InitialVisibilityOption::StartHidden;

    KDDockWidgets::InitialOption options(visibilityOption, dock->preferredSize());

    m_mainWindow->addDockWidget(dockWidgetView, locationToKLocation(location), relativeDockWidgetView, options);
}

void DockWindow::addPanelAsTab(DockPanelView* panel, DockPanelView* destinationPanel)
{
    registerDock(panel);

    if (panel->defaultVisibility()) {
        destinationPanel->addPanelAsTab(panel);
        destinationPanel->setCurrentTabIndex(0);
    }
}

void DockWindow::registerDock(DockBase* dock)
{
    TRACEFUNC;

    IF_ASSERT_FAILED(dock) {
        return;
    }

    auto* registry = KDDockWidgets::DockRegistry::self(iocContext()->id);
    auto* dockWidget = dock->dockWidget();

    if (!registry->containsDockWidget(dockWidget->uniqueName())) {
        registry->registerDockWidget(dockWidget);
    }
}

void DockWindow::handleUnknownDock(const DockPageView* page, DockBase* unknownDock)
{
    DockPanelView* unknownPanel = dynamic_cast<DockPanelView*>(unknownDock);
    if (!unknownPanel) {
        addDock(unknownDock, unknownDock->location(), page->centralDock());
        return;
    }

    if (DockPanelView* destinationPanel = page->findPanelForTab(unknownPanel)) {
        addPanelAsTab(unknownPanel, destinationPanel);
        return;
    }

    DockingHolderView* holder = page->holder(DockType::Panel, unknownPanel->location());
    IF_ASSERT_FAILED(holder) {
        addDock(unknownDock, unknownDock->location(), page->centralDock());
        return;
    }

    registerDock(unknownPanel);

    holder->open(); // init the group...

    auto* holderDockWidgetView = qobject_cast<KDDockWidgets::QtQuick::DockWidget*>(
        KDDockWidgets::QtQuick::asQQuickItem(holder->dockWidget()));
    if (holderDockWidgetView) {
        KDDockWidgets::Core::Group* group = holderDockWidgetView->group();
        if (group) {
            group->addTab(unknownPanel->dockWidget());
        }
    }

    holder->close();

    if (!unknownPanel->isVisible()) {
        unknownPanel->close();
    }
}

DockPageView* DockWindow::pageByUri(const QString& uri) const
{
    for (DockPageView* page : m_pages.list()) {
        if (page->uri() == uri) {
            return page;
        }
    }

    return nullptr;
}

bool DockWindow::doLoadPage(const QString& uri, const QVariantMap& params)
{
    DockPageView* newPage = pageByUri(uri);
    IF_ASSERT_FAILED(newPage) {
        return false;
    }

    newPage->setVisible(true);

    loadPageContent(newPage);
    restorePageState(newPage);
    initDocks(newPage);

    newPage->setParams(params);

    m_currentPage = newPage;

    connect(m_currentPage, &DockPageView::layoutRequested,
            this, &DockWindow::forceLayout, Qt::UniqueConnection);

    return true;
}

void DockWindow::saveWindowGeometry()
{
    /// NOTE: The state of all dock widgets is also saved here,
    /// since the library does not provide the ability to save
    /// and restore only the application geometry.
    uiState()->setWindowGeometry(windowState());
}

void DockWindow::restoreGeometry()
{
    TRACEFUNC;

    if (uiState()->windowGeometry().isEmpty()) {
        return;
    }

    if (restoreLayout(uiState()->windowGeometry())) {
        m_hasGeometryBeenRestored = true;
    } else {
        LOGE() << "Could not restore the window geometry!";
    }
}

void DockWindow::savePageState(const QString& pageName)
{
    TRACEFUNC;

    m_reloadCurrentPageAllowed = false;
    uiState()->setPageState(pageName, windowState());
    m_reloadCurrentPageAllowed = true;
}

void DockWindow::restorePageState(const DockPageView* page)
{
    TRACEFUNC;

    const QString& pageName = page->objectName();

    ValNt<QByteArray> pageStateValNt = uiState()->pageState(pageName);
    const bool layoutIsEmpty = pageStateValNt.val.isEmpty();

    QSet<DockBase*> unknownDocks;
    if (!layoutIsEmpty) {
        for (DockBase* dock : page->allDocks()) {
            const KDDockWidgets::Core::DockWidget* dockWidget = dock->dockWidget();
            if (!pageStateValNt.val.contains(dockWidget->uniqueName().toLocal8Bit())) {
                unknownDocks.insert(dock);
            }
        }
    }

    /// NOTE: Do not restore geometry
    bool ok = restoreLayout(pageStateValNt.val, true /*restoreRelativeToMainWindow*/);
    if (!ok) {
        LOGE() << "Could not restore the state of " << pageName << "!";
    }

    if (!layoutIsEmpty) {
        for (DockBase* dock : unknownDocks) {
            handleUnknownDock(page, dock);
        }
    }

    if (!pageStateValNt.notification.isConnected()) {
        pageStateValNt.notification.onNotify(this, [this, pageName]() {
            bool isCurrentPage = m_currentPage && (m_currentPage->objectName() == pageName);
            if (isCurrentPage) {
                reloadCurrentPage();
            }
        });
    }
}

bool DockWindow::restoreLayout(const QByteArray& layout, bool restoreRelativeToMainWindow)
{
    if (layout.isEmpty()) {
        return true;
    }

    TRACEFUNC;

    auto option = restoreRelativeToMainWindow ? KDDockWidgets::RestoreOption_RelativeToMainWindow
                  : KDDockWidgets::RestoreOption_None;

    KDDockWidgets::LayoutSaver layoutSaver(iocContext()->id, option);
    return layoutSaver.restoreLayout(layout);
}

bool DockWindow::checkLayoutIsCorrupted() const
{
    TRACEFUNC;

    for (const DockBase* dock : m_currentPage->allDocks()) {
        if (!dock) {
            continue;
        }

        if (!dock->floatable() && dock->floating()) {
            return true;
        }
    }

    return false;
}

void DockWindow::forceLayout()
{
    m_mainWindow->layoutEqually();
}

QByteArray DockWindow::windowState() const
{
    TRACEFUNC;

    KDDockWidgets::LayoutSaver layoutSaver(iocContext()->id, KDDockWidgets::RestoreOption_None);
    return layoutSaver.serializeLayout();
}

void DockWindow::reloadCurrentPage()
{
    if (!m_reloadCurrentPageAllowed) {
        return;
    }

    TRACEFUNC;

    clearRegistry(iocContext()->id);

    for (DockBase* dock : m_currentPage->allDocks()) {
        dock->deinit();
    }

    QString currentPageUriBackup = currentPageUri();

    /// NOTE: for reset geometry
    m_currentPage = nullptr;

    if (doLoadPage(currentPageUriBackup)) {
        notifyAboutDocksOpenStatus();
    }
}

void DockWindow::initDocks(DockPageView* page)
{
    TRACEFUNC;

    //! before init we should correct toolbars sizes
    adjustContentForAvailableSpace(page);

    for (DockToolBarView* toolbar : m_toolBars.list()) {
        toolbar->setParentItem(this);
        toolbar->init();
    }

    if (page) {
        page->setParentItem(this);
        page->init();
    }

    alignTopLevelToolBars(page);

    if (!m_pageConnections.contains(page)) {
        m_pageConnections[page] = new UniqueConnectionHolder(page, this);
    }

    UniqueConnectionHolder* holder = m_pageConnections[page];

    for (DockToolBarView* toolbar : topLevelToolBars(page)) {
        connect(toolbar, &DockToolBarView::floatingChanged,
                holder, &UniqueConnectionHolder::alignTopLevelToolBars, Qt::UniqueConnection);

        connect(toolbar, &DockToolBarView::contentSizeChanged,
                holder, &UniqueConnectionHolder::alignTopLevelToolBars, Qt::UniqueConnection);

        connect(toolbar, &DockToolBarView::visibleChanged,
                holder, &UniqueConnectionHolder::alignTopLevelToolBars, Qt::UniqueConnection);
    }
}

void DockWindow::adjustContentForAvailableSpace(DockPageView* page)
{
    if (!page) {
        return;
    }

    int spaceWidth = m_pendingWidth >= 0 ? m_pendingWidth : int(width());

    auto adjustDocks = [&spaceWidth](QList<DockBase*> docks) {
        int width = 0;
        for (DockBase* dock : docks) {
            width += dock->contentWidth();
        }

        docks.erase(std::remove_if(docks.begin(), docks.end(), [](const DockBase* dock){
            return dock->compactPriorityOrder() == -1;
        }), docks.end());

        if (docks.empty()) {
            return;
        }

        std::sort(docks.begin(), docks.end(), [](const DockBase* dock1, DockBase* dock2) {
            return dock1->compactPriorityOrder() < dock2->compactPriorityOrder();
        });

        if (width >= spaceWidth) {
            for (DockBase* dock : docks) {
                if (!dock->isCompact()) {
                    dock->setIsCompact(true);

                    width -= dock->nonCompactWidth();
                    width += dock->width();
                }
            }
        } else {
            for (int i = docks.size() - 1; i >= 0; i--) {
                DockBase* dock = docks.at(i);
                if (!dock->isCompact()) {
                    continue;
                }

                int actualWidth = dock->contentWidth();
                int nonCompactWidth = dock->nonCompactWidth();
                if (width - actualWidth + nonCompactWidth < spaceWidth) {
                    dock->setIsCompact(false);
                }

                break;
            }
        }
    };

    QList<DockBase*> topLevelToolBarsDocks;

    for (DockToolBarView* toolBar : topLevelToolBars(page)) {
        if (!toolBar->dockWidget()->isFloating() && toolBar->isVisible()) {
            topLevelToolBarsDocks << toolBar;
        }
    }

    adjustDocks(topLevelToolBarsDocks);
}

void DockWindow::notifyAboutDocksOpenStatus()
{
    const DockPageView* page = currentPage();

    IF_ASSERT_FAILED(page) {
        return;
    }

    QStringList dockNames;

    for (DockToolBarView* toolBar : page->mainToolBars()) {
        dockNames << toolBar->objectName();
    }

    for (DockToolBarView* toolBar : page->toolBars()) {
        dockNames << toolBar->objectName();
    }

    for (DockPanelView* panel : page->panels()) {
        dockNames << panel->objectName();
    }

    if (page->statusBar()) {
        dockNames << page->statusBar()->objectName();
    }

    m_docksOpenStatusChanged.send(dockNames);
}

QList<DockToolBarView*> DockWindow::topLevelToolBars(const DockPageView* page) const
{
    QList<DockToolBarView*> toolBars = m_toolBars.list();

    if (page) {
        toolBars << page->mainToolBars();
    }

    return toolBars;
}

#include "dockwindow.moc"
