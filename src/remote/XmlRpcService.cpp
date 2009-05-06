/*
FatRat download manager
http://fatrat.dolezel.info

Copyright (C) 2006-2009 Lubos Dolezel <lubos a dolezel.info>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
version 2 as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

In addition, as a special exemption, Luboš Doležel gives permission
to link the code of FatRat with the OpenSSL project's
"OpenSSL" library (or with modified versions of it that use the; same
license as the "OpenSSL" library), and distribute the linked
executables. You must obey the GNU General Public License in all
respects for all of the code used other than "OpenSSL".
*/

#include "XmlRpcService.h"
#include "HttpService.h"
#include "RuntimeException.h"
#include "XmlRpc.h"
#include "Queue.h"
#include <QReadWriteLock>
#include <QtDebug>

extern QList<Queue*> g_queues;
extern QReadWriteLock g_queuesLock;

static void checkArguments(const QList<QVariant>& args, QVariant::Type* types, int ntypes)
{
	if(args.size() != ntypes)
		throw XmlRpcService::XmlRpcError(2, "Invalid argument count");
	for(int i=0;i<ntypes;i++)
	{
		if(args[i].type() != types[i])
			throw XmlRpcService::XmlRpcError(3, QString("Invalid argument type - #%1").arg(i));
	}
}

static void checkType(QVariant var, QVariant::Type type)
{
	if(var.type() != type)
	{
		throw XmlRpcService::XmlRpcError(4, QString("Invalid subargument type - %1 instead of %2")
						 .arg(var.type()).arg(type));
	}
}

void XmlRpcService::serve(QByteArray postData, OutputBuffer* output)
{
	QByteArray data;

	qDebug() << "XML-RPC call:" << postData;

	try
	{
		QByteArray function;
		QList<QVariant> args;
		QVariant returnValue;

		XmlRpc::parseCall(postData, function, args);

		if(function == "getQueues")
			returnValue = getQueues();
		else if(function == "Queue.getTransfers")
		{
			QVariant::Type aa[] = { QVariant::String };
			checkArguments(args, aa, sizeof(aa)/sizeof(aa[0]));

			returnValue = Queue_getTransfers(args[0].toString());
		}
		else if(function == "Transfer.setProperties")
		{
			QVariant::Type aa[] = { QVariant::String, QVariant::Map };
			checkArguments(args, aa, sizeof(aa)/sizeof(aa[0]));

			returnValue = Transfer_setProperties(args[0].toString(), args[1].toMap());
		}
		else
			throw XmlRpcError(1, "Unknown method");

		data = XmlRpc::createResponse(returnValue);
	}
	catch(const XmlRpcError& e) // error to be reported to the client
	{
		data = XmlRpc::createFaultResponse(e.code, e.desc);
	}
	catch(const RuntimeException& e) // syntax error from XmlRpc
	{
		qDebug() << "XmlRpcService::serve():" << e.what();
		throw "400 Bad Request";
	}

	qDebug() << "XML-RPC response:" << data;

	output->putData(data.data(), data.size());
}

QVariant XmlRpcService::getQueues()
{
	QVariantList qlist;
	QReadLocker l(&g_queuesLock);

	for(int i=0;i<g_queues.size();i++)
	{
		Queue* q = g_queues[i];
		QVariantMap vmap;
		int up, down;

		vmap["name"] = q->name();
		vmap["uuid"] = q->uuid();
		vmap["defaultDirectory"] = q->defaultDirectory();
		vmap["moveDirectory"] = q->moveDirectory();
		vmap["upAsDown"] = q->upAsDown();

		q->transferLimits(down, up);
		vmap["transferLimits"] = QVariantList() << down << up;

		q->speedLimits(down, up);
		vmap["speedLimits"] = QVariantList() << down << up;

		qlist << vmap;
	}

	return qlist;
}

QVariant XmlRpcService::Queue_getTransfers(QString uuid)
{
	QReadLocker l(&g_queuesLock);
	Queue* q = 0;
	QVariantList vlist;

	for(int i=0;i<g_queues.size();i++)
	{
		if(g_queues[i]->uuid() == uuid)
		{
			q = g_queues[i];
			break;
		}
	}

	if(!q)
		throw XmlRpcError(101, "Invalid queue UUID");

	q->lock();

	for(int i=0;i<q->size();i++)
	{
		Transfer* t = q->at(i);
		QVariantMap vmap;
		int down, up;

		vmap["name"] = t->name();
		vmap["state"] = Transfer::state2string(t->state());
		vmap["class"] = t->myClass();
		vmap["message"] = t->message();
		vmap["mode"] = (t->mode() == Transfer::Download) ? "Download" : "Upload";
		vmap["primaryMode"] = (t->primaryMode() == Transfer::Download) ? "Download" : "Upload";
		vmap["dataPath"] = t->dataPath();
		vmap["total"] = double(t->total());
		vmap["done"] = double(t->done());
		vmap["uuid"] = t->uuid();
		vmap["comment"] = t->comment();
		vmap["object"] = t->object();
		vmap["timeRunning"] = double(t->timeRunning());

		t->speeds(down, up);
		vmap["speeds"] = QVariantList() << down << up;

		t->userSpeedLimits(down, up);
		vmap["userSpeedLimits"] = QVariantList() << down << up;

		vlist << vmap;
	}

	q->unlock();
	return vlist;
}

QVariant XmlRpcService::Transfer_setProperties(QString uuid, QVariantMap properties)
{
	Queue* q = 0;
	Transfer* t = 0;

	HttpService::findTransfer(uuid, &q, &t);

	if(!t)
		throw XmlRpcError(102, "Invalid transfer UUID");

	for(QVariantMap::const_iterator it = properties.constBegin(); it != properties.constEnd(); it++)
	{
		QString prop = it.key();
		if(prop == "state")
		{
			checkType(it.value(), QVariant::String);
			t->setState(Transfer::string2state(it.value().toString()));
		}
		else if(prop == "comment")
		{
			checkType(it.value(), QVariant::String);
			t->setComment(it.value().toString());
		}
		else if(prop == "object")
		{
			checkType(it.value(), QVariant::String);
			t->setObject(it.value().toString());
		}
		else if(prop == "userSpeedLimits")
		{
			checkType(it.value(), QVariant::List);
			QVariantList list = it.value().toList();

			if(list.size() != 2)
				throw XmlRpcError(104, QString("Invalid list length: %1").arg(list.size()));

			checkType(list.at(0), QVariant::Int);
			checkType(list.at(1), QVariant::Int);

			t->setUserSpeedLimits(list.at(0).toInt(), list.at(1).toInt());
		}
		else
			throw XmlRpcError(103, QString("Invalid transfer property: %1").arg(prop));
	}

	q->unlock();
	return QVariant();
}
