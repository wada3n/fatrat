#include "fatrat.h"
#include "MainWindow.h"
#include "QueueDlg.h"
#include "Queue.h"
#include "FakeDownload.h"
#include "WidgetHostDlg.h"
#include "NewTransferDlg.h"
#include "GenericOptsForm.h"
#include <QHeaderView>
#include <QMessageBox>
#include <QMenu>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <iostream>
#include <QtDebug>
#include "InfoBar.h"
#include "SettingsDlg.h"
#include "SimpleEmail.h"
#include "SpeedGraph.h"
#include "DropBox.h"
#include <stdexcept>

extern QList<Queue*> g_queues;
extern QReadWriteLock g_queuesLock;
extern QSettings* g_settings;

using namespace std;

MainWindow::MainWindow() : m_trayIcon(this), m_pDetailsDisplay(0)
{	
	setupUi();
	
	connect(&m_timer, SIGNAL(timeout()), this, SLOT(updateUi()));
	m_timer.start(1000);
	
	refreshQueues();
	if(g_queues.size())
	{
		listQueues->setCurrentRow(0);
		queueItemActivated(0);
	}
	
	m_graph = new SpeedGraph(this);
	tabMain->insertTab(2, m_graph, QIcon(QString::fromUtf8(":/menu/network.png")), QApplication::translate("MainWindow", "Transfer speed graph", 0, QApplication::UnicodeUTF8));
	m_log = new TransferLog(this, textTransferLog);
	
	updateUi();
}

MainWindow::~MainWindow()
{
	delete m_modelTransfers;
}

void MainWindow::setupUi()
{
	Ui_MainWindow::setupUi(this);
	
	g_settings->beginGroup("state");
	
	m_dropBox = new DropBox(this);
	
	connect(actionAbout, SIGNAL(triggered()), this, SLOT(about()));
	connect(actionNewQueue, SIGNAL(triggered()), this, SLOT(newQueue()));
	connect(actionDeleteQueue, SIGNAL(triggered()), this, SLOT(deleteQueue()));
	connect(actionQueueProperties, SIGNAL(triggered()), this, SLOT(queueItemProperties()));
	
	connect(listQueues, SIGNAL(itemActivated(QListWidgetItem*)), this, SLOT(queueItemActivated(QListWidgetItem*)));
	connect(listQueues, SIGNAL(itemClicked(QListWidgetItem*)), this, SLOT(queueItemActivated(QListWidgetItem*)));
	connect(listQueues, SIGNAL(itemDoubleClicked(QListWidgetItem*)), this, SLOT(queueItemProperties()));
	connect(listQueues, SIGNAL(customContextMenuRequested(const QPoint&)), this, SLOT(queueItemContext(const QPoint&)));
	
	connect(treeTransfers, SIGNAL(activated(const QModelIndex &)), this, SLOT(transferItemActivated(const QModelIndex&)));
	connect(treeTransfers, SIGNAL(clicked(const QModelIndex &)), this, SLOT(transferItemActivated(const QModelIndex&)));
	connect(treeTransfers, SIGNAL(doubleClicked(const QModelIndex &)), this, SLOT(transferItemDoubleClicked(const QModelIndex&)));
	connect(treeTransfers, SIGNAL(customContextMenuRequested(const QPoint&)), this, SLOT(transferItemContext(const QPoint&)));
	
	connect(actionNewTransfer, SIGNAL(triggered()), this, SLOT(addTransfer()));
	connect(actionDeleteTransfer, SIGNAL(triggered()), this, SLOT(deleteTransfer()));
	connect(actionRemoveCompleted, SIGNAL(triggered()), this, SLOT(removeCompleted()));
	
	connect(actionResume, SIGNAL(triggered()), this, SLOT(resumeTransfer()));
	connect(actionForcedResume, SIGNAL(triggered()), this, SLOT(forcedResumeTransfer()));
	connect(actionPause, SIGNAL(triggered()), this, SLOT(pauseTransfer()));
	
	connect(actionTop, SIGNAL(triggered()), this, SLOT(moveToTop()));
	connect(actionUp, SIGNAL(triggered()), this, SLOT(moveUp()));
	connect(actionDown, SIGNAL(triggered()), this, SLOT(moveDown()));
	connect(actionBottom, SIGNAL(triggered()), this, SLOT(moveToBottom()));
	connect(actionResumeAll, SIGNAL(triggered()), this, SLOT(resumeAll()));
	connect(actionStopAll, SIGNAL(triggered()), this, SLOT(stopAll()));
	
	connect(actionDropBox, SIGNAL(toggled(bool)), m_dropBox, SLOT(setVisible(bool)));
	connect(actionInfoBar, SIGNAL(toggled(bool)), this, SLOT(toggleInfoBar(bool)));
	connect(actionProperties, SIGNAL(triggered()), this, SLOT(transferOptions()));
	connect(actionDisplay, SIGNAL(toggled(bool)), this, SLOT(setVisible(bool)));
	connect(actionHideAllInfoBars, SIGNAL(triggered()),this, SLOT(hideAllInfoBars()));
	
	connect(pushGenericOptions, SIGNAL(clicked()), this, SLOT(transferOptions()));
	connect(tabMain, SIGNAL(currentChanged(int)), this, SLOT(currentTabChanged(int)));
	
	connect(actionQuit, SIGNAL(triggered()), qApp, SLOT(quit()));
	connect(actionAboutQt, SIGNAL(triggered()), qApp, SLOT(aboutQt()));
	connect(&m_trayIcon, SIGNAL(activated(QSystemTrayIcon::ActivationReason)), this, SLOT(trayIconActivated(QSystemTrayIcon::ActivationReason)));
	connect(actionSettings, SIGNAL(triggered()), this, SLOT(showSettings()));
	
	connect(TransferNotifier::instance(), SIGNAL(stateChanged(Transfer*,Transfer::State,Transfer::State)), this, SLOT(downloadStateChanged(Transfer*,Transfer::State,Transfer::State)));
	
	m_modelTransfers = new TransfersModel(this);
	treeTransfers->setModel(m_modelTransfers);
	treeTransfers->setItemDelegate(new ProgressDelegate);
	
	//////////////////////////
	// RESTORE WINDOW STATE //
	//////////////////////////
	{
		QHeaderView* hdr = treeTransfers->header();
		QVariant state = g_settings->value("mainheaders");
		
		if(state.isNull())
		{
			hdr->resizeSection(0, 300);
			hdr->resizeSection(3, 160);
		}
		else
			hdr->restoreState(state.toByteArray());
		
		state = g_settings->value("mainsplitter");
		
		if(state.isNull())
			splitterQueues->setSizes(QList<int>() << 80 << 600);
		else
			splitterQueues->restoreState(state.toByteArray());
		
		connect(hdr, SIGNAL(sectionResized(int,int,int)), this, SLOT(saveWindowState()));
		connect(splitterQueues, SIGNAL(splitterMoved(int,int)), this, SLOT(saveWindowState()));
	}
	
	treeTransfers->setContextMenuPolicy(Qt::CustomContextMenu);
	listQueues->setContextMenuPolicy(Qt::CustomContextMenu);
	
	m_trayIcon.setIcon(QIcon(":/fatrat/fatrat.png"));
	m_trayIcon.setToolTip("FatRat");
	showTrayIcon();
	
	statusbar->addWidget(&m_labelStatus);
}

void MainWindow::saveWindowState()
{
	g_settings->beginGroup("state");
	
	g_settings->setValue("mainheaders", treeTransfers->header()->saveState());
	g_settings->setValue("mainsplitter", splitterQueues->saveState());
	g_settings->sync();
}

void MainWindow::about()
{
	QMessageBox::about(this, tr("About FatRat"),
				QString::fromUtf8("<b>FatRat download manager</b><p>"
				"Copyright © 2006-2007 Luboš Doležel &lt;lubos@dolezel.info&gt;<p>"
				"Copyright © 2004-2006 <a href=\"http://www.trolltech.com\">Trolltech ASA</a><br>"
				"Copyright © 2001 The Internet Society. All Rights Reserved.<p>"
				"Web: <a href=\"http://fatrat.dolezel.info\">http://fatrat.dolezel.info</a><p>"
				"Licensed under terms of the GNU GPL v2 license.")
			);
}

void MainWindow::trayIconActivated(QSystemTrayIcon::ActivationReason reason)
{
	if(reason == QSystemTrayIcon::Trigger)
	{
		actionDisplay->toggle();
	}
	else if(reason == QSystemTrayIcon::Context)
	{
		QMenu menu(this);
		
		menu.addAction(actionDisplay);
		menu.addAction(actionDropBox);
		menu.addSeparator();
		menu.addAction(actionHideAllInfoBars);
		menu.addSeparator();
		menu.addAction(actionQuit);
		
		menu.exec(QCursor::pos());
	}
	else if(reason == QSystemTrayIcon::MiddleClick)
	{
		actionDropBox->toggle();
	}
}

void MainWindow::hideAllInfoBars()
{
	InfoBar::removeAll();
}

void MainWindow::hideEvent(QHideEvent* event)
{
	if(isMinimized() && m_trayIcon.isVisible() && g_settings->value("hideminimize", false).toBool())
	{
		actionDisplay->setChecked(false);
		hide();
	}
	else
		QWidget::hideEvent(event);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
	if(m_trayIcon.isVisible() && g_settings->value("hideclose", true).toBool())
	{
		actionDisplay->setChecked(false);
		hide();
	}
	else
	{
		m_trayIcon.hide();
		QWidget::closeEvent(event);
		qApp->quit();
	}
}

void MainWindow::updateUi()
{
	QModelIndex ctrans = treeTransfers->currentIndex();
	
	if(listQueues->currentItem())
	{
		actionDeleteQueue->setEnabled(true);
		actionQueueProperties->setEnabled(true);
		actionNewTransfer->setEnabled(true);
	}
	else
	{
		actionDeleteQueue->setEnabled(false);
		actionQueueProperties->setEnabled(false);
		actionNewTransfer->setEnabled(false);
	}
	
	Queue* q = getCurrentQueue();
	if(ctrans.isValid() && q != 0 && ctrans.row() < q->size())
	{
		int rcount = m_modelTransfers->rowCount();
		
		tabMain->setTabEnabled(1,true); // transfer details
		tabMain->setTabEnabled(2,true); // transfer graph
		tabMain->setTabEnabled(3,true); // transfer log
		
		actionTop->setEnabled(ctrans.row());
		actionUp->setEnabled(ctrans.row());
		actionDown->setEnabled(ctrans.row() < rcount-1);
		actionBottom->setEnabled(ctrans.row() < rcount-1);
		actionDeleteTransfer->setEnabled(true);
		
		actionInfoBar->setEnabled(true);
		actionProperties->setEnabled(true);
		
		q->lock();
		
		Transfer* d = q->at(ctrans.row());
		actionInfoBar->setChecked(InfoBar::getInfoBar(d) != 0);
		
		actionForcedResume->setEnabled(d->statePossible(Transfer::ForcedActive));
		actionResume->setEnabled(d->statePossible(Transfer::Active));
		actionPause->setEnabled(d->statePossible(Transfer::Paused));
		
		m_graph->setRenderSource(d);
		m_log->setLogSource(d);
		
		q->unlock();
	}
	else
	{
		tabMain->setTabEnabled(1,false); // transfer details
		tabMain->setTabEnabled(2,false); // transfer graph
		tabMain->setTabEnabled(3,false); // transfer log
		
		actionTop->setEnabled(false);
		actionUp->setEnabled(false);
		actionDown->setEnabled(false);
		actionBottom->setEnabled(false);
		actionDeleteTransfer->setEnabled(false);
		
		actionForcedResume->setEnabled(false);
		actionResume->setEnabled(false);
		actionPause->setEnabled(false);
		
		actionInfoBar->setEnabled(false);
		actionProperties->setEnabled(false);
		
		m_graph->setRenderSource(0);
		m_log->setLogSource(0);
	}
	if(q != 0)
		doneCurrentQueue(q,true,false);
	
	refreshQueues();
	m_modelTransfers->refresh();
	if(tabMain->currentIndex() == 1)
		refreshDetailsTab();
}

void MainWindow::refreshQueues()
{
	g_queuesLock.lockForRead();
	
	int i;
	for(i=0;i<g_queues.size();i++)
	{
		if(i>=listQueues->count())
			listQueues->addItem(g_queues[i]->name());
		else
			listQueues->item(i)->setText(g_queues[i]->name());
	}
	
	while(i<listQueues->count())
		delete listQueues->takeItem(i);
	
	int upt = 0,downt = 0;
	if(Queue* q = getCurrentQueue(false))
	{
		for(int i=0;i<q->size();i++)
		{
			int up,down;
			q->at(i)->speeds(down,up);
			downt += down;
			upt += up;
		}
		doneCurrentQueue(q,false,false);
	}
	g_queuesLock.unlock();
	
	if(upt || downt)
		m_labelStatus.setText( QString(tr("Speed of the selected queue: %1 down, %2 up")).arg(formatSize(downt,true)).arg(formatSize(upt,true)) );
	else
		m_labelStatus.setText(QString());
}

void MainWindow::newQueue()
{
	QueueDlg dlg(this);
	
	if(dlg.exec() == QDialog::Accepted)
	{
		Queue* q = new Queue;
		
		q->setName(dlg.m_strName);
		q->setSpeedLimits(dlg.m_nDownLimit,dlg.m_nUpLimit);
		if(dlg.m_bLimit)
			q->setTransferLimits(dlg.m_nDownTransfersLimit,dlg.m_nUpTransfersLimit);
		q->setUpAsDown(dlg.m_bUpAsDown);
		
		g_queuesLock.lockForWrite();
		g_queues << q;
		g_queuesLock.unlock();
		
		Queue::saveQueues();
		updateUi();
	}
}

void MainWindow::deleteQueue()
{
	if(g_queues.empty()) return;
	
	if(QMessageBox::warning(this, tr("Delete queue"), tr("Do you really want to delete the active queue?"), QMessageBox::Yes|QMessageBox::No) == QMessageBox::Yes)
	{
		g_queuesLock.lockForWrite();
		g_queues.removeAt(listQueues->currentRow());
		g_queuesLock.unlock();
		
		Queue::saveQueues();
		updateUi();
		queueItemActivated(0);
	}
}

void MainWindow::queueItemActivated(QListWidgetItem*)
{
	m_modelTransfers->setQueue(listQueues->currentRow());
	
	if(m_pDetailsDisplay)
		m_pDetailsDisplay->deleteLater();
}

void MainWindow::queueItemProperties()
{
	QueueDlg dlg(this);
	g_queuesLock.lockForRead();
	
	Queue* q = g_queues[listQueues->currentRow()];
	
	dlg.m_strName = q->name();
	dlg.m_bUpAsDown = q->upAsDown();
	q->speedLimits(dlg.m_nDownLimit, dlg.m_nUpLimit);
	q->transferLimits(dlg.m_nDownTransfersLimit,dlg.m_nUpTransfersLimit);
	
	if(dlg.m_nDownTransfersLimit < 0 || dlg.m_nUpTransfersLimit < 0)
	{
		dlg.m_nDownLimit = dlg.m_nUpLimit = 1;
		dlg.m_bLimit = false;
	}
	else
		dlg.m_bLimit = true;
	
	if(dlg.exec() == QDialog::Accepted)
	{
		q->setName(dlg.m_strName);
		q->setSpeedLimits(dlg.m_nDownLimit,dlg.m_nUpLimit);
		if(dlg.m_bLimit)
			q->setTransferLimits(dlg.m_nDownTransfersLimit,dlg.m_nUpTransfersLimit);
		else
			q->setTransferLimits();
		q->setUpAsDown(dlg.m_bUpAsDown);
		listQueues->currentItem()->setText(dlg.m_strName);
		
		Queue::saveQueues();
	}
	
	g_queuesLock.unlock();
}

void MainWindow::queueItemContext(const QPoint&)
{
	if(listQueues->currentRow() != -1)
	{
		QMenu menu(listQueues);
		
		menu.addAction(actionResumeAll);
		menu.addAction(actionStopAll);
		menu.addSeparator();
		menu.addAction(actionQueueProperties);
		
		updateUi();
		menu.exec(QCursor::pos());
	}
}

void MainWindow::transferItemActivated(const QModelIndex&)
{
	updateUi();
	
	if(m_pDetailsDisplay)
		m_pDetailsDisplay->deleteLater();
	else
		displayDestroyed();
}

void MainWindow::displayDestroyed()
{
	Queue* q = getCurrentQueue();
	QModelIndex i = treeTransfers->currentIndex();
	Transfer* d = q->at(i.row());
	
	qDebug() << "MainWindow::displayDestroyed()";
	
	if(QWidget* w = stackedDetails->currentWidget())
		stackedDetails->removeWidget(w);
	m_pDetailsDisplay = 0;
	
	if(d != 0)
	{
		QWidget* widgetDisplay = new QWidget(stackedDetails);
		m_pDetailsDisplay = d->createDetailsWidget(widgetDisplay);
		if(m_pDetailsDisplay)
		{
			stackedDetails->insertWidget(0,widgetDisplay);
			stackedDetails->setCurrentIndex(0);
			
			connect(m_pDetailsDisplay, SIGNAL(destroyed()), this, SLOT(displayDestroyed()));
		}
		else
			delete widgetDisplay;
	}
	
	doneCurrentQueue(q);
}

void MainWindow::transferItemDoubleClicked(const QModelIndex&)
{
	tabMain->setCurrentIndex(1);
}

void MainWindow::move(int i)
{
	Queue* q = getCurrentQueue(false);
	QModelIndex ctrans = treeTransfers->currentIndex();
	
	if(!q) return;
	
	if(ctrans.isValid())
	{
		switch(i)
		{
		case 0:
			q->moveToTop(ctrans.row()); treeTransfers->setCurrentIndex(m_modelTransfers->index(0)); break;
		case 1:
			q->moveUp(ctrans.row()); treeTransfers->setCurrentIndex(m_modelTransfers->index(ctrans.row()-1)); break;
		case 2:
			q->moveDown(ctrans.row()); treeTransfers->setCurrentIndex(m_modelTransfers->index(ctrans.row()+1)); break;
		case 3:
			q->moveToBottom(ctrans.row()); treeTransfers->setCurrentIndex(m_modelTransfers->index(q->size()-1)); break;
		}
	}
	
	doneCurrentQueue(q,false);
	Queue::saveQueues();
}

void MainWindow::moveToTop()
{
	move(0);
}

void MainWindow::moveUp()
{
	move(1);
}

void MainWindow::moveDown()
{
	move(2);
}

void MainWindow::moveToBottom()
{
	move(3);
}

void MainWindow::addTransfer(QString uri)
{
	if(!listQueues->currentItem()) return;
	
	NewTransferDlg dlg(this);
	Queue* queue = 0;
	QList<Transfer*> listTransfers;
	
	dlg.setWindowTitle(tr("New transfer"));
	
	dlg.m_strURIs = uri;
	
	if(uri.startsWith("file:/") || uri.startsWith("/"))
		dlg.m_mode = Transfer::Upload;
	
	if(dlg.exec() != QDialog::Accepted)
		return;
	
	try
	{
		QStringList uris = dlg.m_strURIs.split(QRegExp("\\s+"), QString::SkipEmptyParts);
		
		if(uris.isEmpty())
			return;
		
		if(dlg.m_nClass == -1)
		{
			// autodetection
			const EngineEntry* entries;
			int curscore = 0;
			
			entries = Transfer::engines(dlg.m_mode);
			
			for(int i=0;entries[i].shortName;i++)
			{
				int n;
				
				if(dlg.m_mode == Transfer::Download)
					n = entries[i].lpfnAcceptable(uris[0]);
				else
					n = entries[i].lpfnAcceptable(dlg.m_strDestination);
				if(n > curscore)
				{
					curscore = n;
					dlg.m_nClass = i;
				}
			}
			
			if(dlg.m_nClass == -1)
			{
				QMessageBox::critical(this, tr("Error"), tr("Couldn't autodetect transfer type."));
				return;
			}
		}
		
		queue = getCurrentQueue(false);
		for(int i=0;i<uris.size();i++)
		{
			cout << "Class ID: " << dlg.m_nClass << endl;
			Transfer* d = Transfer::createInstance(dlg.m_mode, dlg.m_nClass);
			
			if(d == 0)
				throw std::runtime_error("Failed to create class instance.");
			
			listTransfers << d;
			
			if(dlg.m_bPaused)
				d->setState(Transfer::Paused);
			d->init(uris[i].trimmed(),dlg.m_strDestination);
			d->setUserSpeedLimits(dlg.m_nDownLimit,dlg.m_nUpLimit);
		}
		
		// show the transfer details dialog
		if(dlg.m_bDetails)
		{
			// show a typical transfer propeties dialog
			if(listTransfers.size() == 1)
			{
				WidgetHostDlg dlg(this);
				dlg.setWindowTitle(tr("Transfer details"));
				
				if(WidgetHostChild* q = listTransfers[0]->createOptionsWidget(dlg.getChildHost()))
				{
					dlg.addChild(q);
					
					if(dlg.exec() != QDialog::Accepted)
						throw std::runtime_error(std::string());
				}
			}
			else // show a dialog designed for multiple
			{
				if(!Transfer::runProperties(this, dlg.m_mode, dlg.m_nClass, listTransfers))
					throw std::runtime_error(std::string());
			}
		}
		
		queue->add(listTransfers);
	}
	catch(const std::exception& e)
	{
		qDeleteAll(listTransfers);
		if(e.what()[0])
			QMessageBox::critical(this, tr("Error"), e.what());
	}
	
	if(queue != 0)
		doneCurrentQueue(queue,false);
	
	Queue::saveQueues();
}

void MainWindow::deleteTransfer()
{
	Queue* q = getCurrentQueue(false);
	QModelIndex ctrans = treeTransfers->currentIndex();
	
	if(!q) return;
	
	if(ctrans.isValid())
	{
		if(QMessageBox::warning(this, tr("Delete transfer"),
			tr("Do you really want to delete the selected transfer?"),
			QMessageBox::Yes|QMessageBox::No) == QMessageBox::Yes)
		{
			q->remove(ctrans.row());
			Queue::saveQueues();
		}
	}
	
	doneCurrentQueue(q,false);
}

void MainWindow::resumeTransfer()
{
	Queue* q = getCurrentQueue();
	Transfer* d;
	QModelIndex ctrans = treeTransfers->currentIndex();
	
	if(!q) return;
	
	d = q->at(ctrans.row());
	if(d->state() == Transfer::ForcedActive)
		d->setState(Transfer::Active);
	else
		d->setState(Transfer::Waiting);
	
	doneCurrentQueue(q);
}

void MainWindow::forcedResumeTransfer()
{
	setTransfer(Transfer::ForcedActive);
}

void MainWindow::pauseTransfer()
{
	setTransfer(Transfer::Paused);
}

void MainWindow::setTransfer(Transfer::State state)
{
	Queue* q = getCurrentQueue();
	Transfer* d;
	QModelIndex ctrans = treeTransfers->currentIndex();
	
	if(!q) return;
	
	d = q->at(ctrans.row());
	if(d->state() != Transfer::Active || state != Transfer::Waiting)
		d->setState(state);
	
	doneCurrentQueue(q);
}

void MainWindow::transferOptions()
{
	cout << "Showing transfer properties\n";
	WidgetHostDlg dlg(this);
	GenericOptsForm wgt(dlg.getNextChildHost(tr("Generic options")));
	
	Queue* q = getCurrentQueue();
	Transfer* d;
	QModelIndex ctrans = treeTransfers->currentIndex();
	QWidget* details = dlg.getNextChildHost(tr("Details"));
	
	if(!q) return;
	
	d = q->at(ctrans.row());
	
	if(d != 0)
	{
		wgt.m_mode = d->primaryMode();
		wgt.m_strURI = d->object();
		
		d->userSpeedLimits(wgt.m_nDownLimit,wgt.m_nUpLimit);
		
		dlg.setWindowTitle(tr("Transfer properties"));
		dlg.addChild(&wgt);
		if(WidgetHostChild* c = d->createOptionsWidget(details))
			dlg.addChild(c);
		else
			dlg.removeChildHost(details);
		
		if(dlg.exec() == QDialog::Accepted)
		{
			try
			{
				d->setObject(wgt.m_strURI);
			}
			catch(const std::exception& e)
			{
				QMessageBox::critical(this, tr("Error"), e.what());
			}
			d->setUserSpeedLimits(wgt.m_nDownLimit,wgt.m_nUpLimit);
			updateUi();
			Queue::saveQueues();
		}
		
		delete details;
	}
	doneCurrentQueue(q);
}

void MainWindow::refreshDetailsTab()
{	
	Queue* q;
	Transfer* d;
	QModelIndex ctrans = treeTransfers->currentIndex();
	QString progress;
	
	if((q = getCurrentQueue()) == 0)
	{
		tabMain->setCurrentIndex(0);
		return;
	}
	
	if(ctrans.row() == -1)
	{
		doneCurrentQueue(q);
		tabMain->setCurrentIndex(0);
		return;
	}
	
	d = q->at(ctrans.row());
	
	lineName->setText(d->name());
	lineMessage->setText(d->message());
	
	if(d->total())
		progress = QString(tr("transfered %1 from %2 (%3%)")).arg(formatSize(d->done())).arg(formatSize(d->total())).arg(100.0/d->total()*d->done(), 0, 'f', 1);
	else
		progress = QString(tr("transfered %1, total size unknown")).arg(formatSize(d->done()));
	
	if(d->isActive())
	{
		int down,up;
		d->speeds(down,up);
		Transfer::Mode mode = d->primaryMode();
		QString speed;
		
		if(down)
			speed = QString("%1 kB/s down ").arg(double(down)/1024.f, 0, 'f', 1);
		if(up)
			speed += QString("%1 kB/s up").arg(double(up)/1024.f, 0, 'f', 1);
		
		if(d->total())
		{
			QString s;
			qulonglong totransfer = d->total() - d->done();
			
			if(down && mode == Transfer::Download)
				progress += QString(tr(", %1 left")).arg( formatTime(totransfer/down) );
			else if(up && mode == Transfer::Upload)
				progress += QString(tr(", %1 left")).arg( formatTime(totransfer/up) );
		}
		
		labelSpeed->setText( speed );
	}
	else
		labelSpeed->setText( QString() );
	
	labelProgress->setText( progress );
	
	doneCurrentQueue(q,true,false);
}

void MainWindow::currentTabChanged(int newTab)
{
	if(newTab == 1)
		refreshDetailsTab();
}

void MainWindow::removeCompleted()
{
	Queue* q;
	QString progress;
	
	if(listQueues->currentRow() == -1)
	{
		tabMain->setCurrentIndex(0);
		return;
	}
	
	g_queuesLock.lockForRead();
	q = g_queues[listQueues->currentRow()];
	q->lockW();
	
	for(int i=0;i<q->size();i++)
	{
		qDebug() << "Remove one\n";
		Transfer* d = q->at(i);
		if(d->state() == Transfer::Completed)
			q->remove(i--,true);
	}
	
	q->unlock();
	g_queuesLock.unlock();
	updateUi();
}

void MainWindow::resumeAll()
{
	changeAll(true);
}

void MainWindow::stopAll()
{
	changeAll(false);
}

void MainWindow::changeAll(bool resume)
{
	Queue* q = getCurrentQueue();
	if(!q) return;
	
	for(int i=0;i<q->size();i++)
	{
		Transfer::State state = q->at(i)->state();
		
		if((state == Transfer::Paused || state == Transfer::Failed) && resume)
			q->at(i)->setState(Transfer::Waiting);
		else if(!resume && state != Transfer::Completed)
			q->at(i)->setState(Transfer::Paused);
	}
	
	doneCurrentQueue(q);
}

Queue* MainWindow::getCurrentQueue(bool lock)
{
	int row;
	Queue* q;
	
	g_queuesLock.lockForRead();
	if((row = listQueues->currentRow()) == -1)
	{
		g_queuesLock.unlock();
		return 0;
	}
	
	if(row >= 0 && row < g_queues.size())
	{
		q = g_queues[row];
		if(lock)
			q->lock();
		return q;
	}
	else
	{
		g_queuesLock.unlock();
		return 0;
	}
}

void MainWindow::doneCurrentQueue(Queue* q, bool unlock, bool refresh)
{
	if(unlock)
		q->unlock();
	g_queuesLock.unlock();
	if(refresh)
		updateUi();
}

void MainWindow::transferItemContext(const QPoint&)
{
	if(treeTransfers->currentIndex().row() != -1)
	{
		QMenu menu(treeTransfers);
		
		menu.addAction(actionResume);
		menu.addAction(actionForcedResume);
		menu.addAction(actionPause);
		menu.addSeparator();
		menu.addAction(actionTop);
		menu.addAction(actionUp);
		menu.addAction(actionDown);
		menu.addAction(actionBottom);
		menu.addSeparator();
		
		QModelIndex ctrans = treeTransfers->currentIndex();
		Queue* q = getCurrentQueue();
		if(q != 0)
		{
			if(ctrans.row() >= 0)
			{
				Transfer* t = q->at(ctrans.row());
				t->fillContextMenu(menu);
			}
			doneCurrentQueue(q, true, false);
		}
		
		menu.addSeparator();
		menu.addAction(actionInfoBar);
		menu.addSeparator();
		menu.addAction(actionProperties);
		
		updateUi();
		menu.exec(QCursor::pos());
	}
}

void MainWindow::toggleInfoBar(bool show)
{
	if(Queue* q = getCurrentQueue())
	{
		q->lock();
		
		QModelIndex ctrans = treeTransfers->currentIndex();
		Transfer* d = q->at(ctrans.row());
		
		if(d != 0)
		{
			if(show)
				new InfoBar(this,d);
			else
				delete InfoBar::getInfoBar(d);
		}
		
		q->unlock();
		doneCurrentQueue(q,true,false);
	}
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
	if(event->mimeData()->hasFormat("text/plain"))
		event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
	event->acceptProposedAction();
	addTransfer(event->mimeData()->text());
}

void MainWindow::showSettings()
{
	SettingsDlg dlg(this);
	if(dlg.exec() == QDialog::Accepted)
		showTrayIcon();
}

void MainWindow::showTrayIcon()
{
	m_trayIcon.setVisible(g_settings->value("trayicon", true).toBool());
}

void MainWindow::downloadStateChanged(Transfer* d, Transfer::State prev, Transfer::State now)
{
	const bool popup = g_settings->value("showpopup", true).toBool();
	const bool email = g_settings->value("sendemail", false).toBool();
	
	cout << "MainWindow::downloadStateChanged()\n";
	
	if(!popup && !email)
		return;
	
	if((prev == Transfer::Active || prev == Transfer::ForcedActive) && (now == Transfer::Completed))
	{
		if(popup)
		{
			m_trayIcon.showMessage(tr("Transfer completed"),
					QString(tr("The transfer of \"%1\" has been completed.")).arg(d->name()),
					QSystemTrayIcon::Information, g_settings->value("popuptime",int(4)).toInt()*1000);
		}
		if(email)
		{
			QString from,to;
			QString message;
			
			from = g_settings->value("emailsender", "root@localhost").toString();
			to = g_settings->value("emailrcpt").toString();
			
			message = QString("From: <%1>\r\nTo: <%2>\r\nSubject: FatRat transfer completed\r\nX-Mailer: FatRat/SVN\r\nThe transfer of \"%3\" has been completed.").arg(from).arg(to).arg(d->name());
			
			new SimpleEmail(g_settings->value("smtpserver","localhost").toString(),from,to,message);
		}
	}
}

void MainWindow::downloadModeChanged(Transfer* d, Transfer::State prev, Transfer::State now)
{
}

