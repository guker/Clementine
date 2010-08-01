/* This file is part of Clementine.

   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "config.h"
#include "devicedatabasebackend.h"
#include "devicekitlister.h"
#include "devicemanager.h"
#include "devicestatefiltermodel.h"
#include "filesystemdevice.h"
#include "core/musicstorage.h"
#include "core/taskmanager.h"
#include "core/utilities.h"
#include "ui/iconloader.h"

#ifdef Q_OS_DARWIN
#  include "macdevicelister.h"
#endif
#ifdef HAVE_LIBGPOD
#  include "gpoddevice.h"
#endif
#ifdef HAVE_GIO
#  include "giolister.h"
#endif
#ifdef HAVE_IMOBILEDEVICE
#  include "ilister.h"
#endif

#include <QIcon>
#include <QPainter>
#include <QSortFilterProxyModel>
#include <QUrl>

const int DeviceManager::kDeviceIconSize = 32;
const int DeviceManager::kDeviceIconOverlaySize = 16;


DeviceManager::DeviceInfo::DeviceInfo()
  : database_id_(-1),
    task_percentage_(-1)
{
}

DeviceDatabaseBackend::Device DeviceManager::DeviceInfo::SaveToDb() const {
  DeviceDatabaseBackend::Device ret;
  ret.friendly_name_ = friendly_name_;
  ret.size_ = size_;
  ret.id_ = database_id_;
  ret.icon_name_ = icon_name_;

  QStringList unique_ids;
  foreach (const Backend& backend, backends_) {
    unique_ids << backend.unique_id_;
  }
  ret.unique_id_ = unique_ids.join(",");

  return ret;
}

void DeviceManager::DeviceInfo::InitFromDb(const DeviceDatabaseBackend::Device &dev) {
  database_id_ = dev.id_;
  friendly_name_ = dev.friendly_name_;
  size_ = dev.size_;
  LoadIcon(dev.icon_name_.split(','), friendly_name_);

  QStringList unique_ids = dev.unique_id_.split(',');
  foreach (const QString& id, unique_ids) {
    backends_ << Backend(NULL, id);
  }
}

void DeviceManager::DeviceInfo::LoadIcon(const QStringList& icons, const QString& name_hint) {
  if (icons.isEmpty()) {
    icon_name_ = "drive-removable-media-usb-pendrive";
    icon_ = IconLoader::Load(icon_name_);
    return;
  }

  // Try to load the icon with that exact name first
  foreach (const QString& name, icons) {
    icon_ = IconLoader::Load(name);
    if (!icon_.isNull()) {
      icon_name_ = name;
      return;
    }
  }

  QString hint = QString(icons.first() + name_hint).toLower();

  // If that failed than try to guess if it's a phone or ipod.  Fall back on
  // a usb memory stick icon.
  if (hint.contains("phone"))
    icon_name_ = "phone";
  else if (hint.contains("ipod") || hint.contains("apple"))
    icon_name_ = "multimedia-player-ipod-standard-monochrome";
  else
    icon_name_ = "drive-removable-media-usb-pendrive";
  icon_ = IconLoader::Load(icon_name_);
}

const DeviceManager::DeviceInfo::Backend* DeviceManager::DeviceInfo::BestBackend() const {
  int best_priority = -1;
  const Backend* ret = NULL;

  for (int i=0 ; i<backends_.count() ; ++i) {
    if (backends_[i].lister_ && backends_[i].lister_->priority() > best_priority) {
      best_priority = backends_[i].lister_->priority();
      ret = &(backends_[i]);
    }
  }

  if (!ret && !backends_.isEmpty())
    return &(backends_[0]);
  return ret;
}


DeviceManager::DeviceManager(BackgroundThread<Database>* database,
                             TaskManager* task_manager, QObject *parent)
  : QAbstractListModel(parent),
    database_(database),
    task_manager_(task_manager),
    not_connected_overlay_(IconLoader::Load("edit-delete"))
{
  connect(task_manager_, SIGNAL(TasksChanged()), SLOT(TasksChanged()));

  // Create the backend in the database thread
  backend_ = database_->CreateInThread<DeviceDatabaseBackend>();
  backend_->Init(database_->Worker());

  DeviceDatabaseBackend::DeviceList devices = backend_->GetAllDevices();
  foreach (const DeviceDatabaseBackend::Device& device, devices) {
    DeviceInfo info;
    info.InitFromDb(device);
    devices_ << info;
  }

  // This proxy model only shows connected devices
  connected_devices_model_ = new DeviceStateFilterModel(this);
  connected_devices_model_->setSourceModel(this);

#ifdef Q_WS_X11
  AddLister(new DeviceKitLister);
#endif
#ifdef HAVE_GIO
  AddLister(new GioLister);
#endif
#ifdef Q_OS_DARWIN
  AddLister(new MacDeviceLister);
#endif
#ifdef HAVE_IMOBILEDEVICE
  AddLister(new iLister);
#endif

  AddDeviceClass<FilesystemDevice>();

#ifdef HAVE_LIBGPOD
  AddDeviceClass<GPodDevice>();
#endif
}

DeviceManager::~DeviceManager() {
  qDeleteAll(listers_);
  backend_->deleteLater();
}

int DeviceManager::rowCount(const QModelIndex&) const {
  return devices_.count();
}

QVariant DeviceManager::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.column() != 0)
    return QVariant();

  const DeviceInfo& info = devices_[index.row()];

  switch (role) {
    case Qt::DisplayRole: {
      QString text;
      if (!info.friendly_name_.isEmpty())
        text = info.friendly_name_;
      else
        text = info.BestBackend()->unique_id_;

      if (info.size_)
        text = text + QString(" (%1)").arg(Utilities::PrettySize(info.size_));
      return text;
    }

    case Qt::DecorationRole: {
      QPixmap pixmap = info.icon_.pixmap(kDeviceIconSize);

      if (!info.BestBackend()->lister_) {
        // Disconnected but remembered
        QPainter p(&pixmap);
        p.drawPixmap(kDeviceIconSize - kDeviceIconOverlaySize,
                     kDeviceIconSize - kDeviceIconOverlaySize,
                     not_connected_overlay_.pixmap(kDeviceIconOverlaySize));
      }

      return pixmap;
    }

    case Role_FriendlyName:
      return info.friendly_name_;

    case Role_UniqueId:
      return info.BestBackend()->unique_id_;

    case Role_IconName:
      return info.icon_name_;

    case Role_Capacity:
    case MusicStorage::Role_Capacity:
      return info.size_;

    case Role_FreeSpace:
    case MusicStorage::Role_FreeSpace:
      return info.BestBackend()->lister_ ?
          info.BestBackend()->lister_->DeviceFreeSpace(info.BestBackend()->unique_id_) :
          QVariant();

    case Role_State:
      if (info.device_)
        return State_Connected;
      if (info.BestBackend()->lister_)
        return State_NotConnected;
      return State_Remembered;

    case Role_UpdatingPercentage:
      if (info.task_percentage_ == -1)
        return QVariant();
      return info.task_percentage_;

    case MusicStorage::Role_Storage:
      if (!info.device_)
        const_cast<DeviceManager*>(this)->Connect(index.row());
      if (!info.device_)
        return QVariant();
      return QVariant::fromValue<MusicStorage*>(info.device_.get());

    case Role_MountPath:
      if (!info.device_)
        return QVariant();
      return info.device_->url().path();

    default:
      return QVariant();
  }
}

void DeviceManager::AddLister(DeviceLister *lister) {
  listers_ << lister;
  connect(lister, SIGNAL(DeviceAdded(QString)), SLOT(PhysicalDeviceAdded(QString)));
  connect(lister, SIGNAL(DeviceRemoved(QString)), SLOT(PhysicalDeviceRemoved(QString)));
  connect(lister, SIGNAL(DeviceChanged(QString)), SLOT(PhysicalDeviceChanged(QString)));

  lister->Start();
}

int DeviceManager::FindDeviceById(const QString &id) const {
  for (int i=0 ; i<devices_.count() ; ++i) {
    foreach (const DeviceInfo::Backend& backend, devices_[i].backends_) {
      if (backend.unique_id_ == id)
        return i;
    }
  }
  return -1;
}

int DeviceManager::FindDeviceByUrl(const QList<QUrl>& urls) const {
  if (urls.isEmpty())
    return -1;

  for (int i=0 ; i<devices_.count() ; ++i) {
    foreach (const DeviceInfo::Backend& backend, devices_[i].backends_) {
      if (!backend.lister_)
        continue;

      QList<QUrl> device_urls = backend.lister_->MakeDeviceUrls(backend.unique_id_);
      foreach (const QUrl& url, device_urls) {
        if (urls.contains(url))
          return i;
      }
    }
  }
  return -1;
}

void DeviceManager::PhysicalDeviceAdded(const QString &id) {
  DeviceLister* lister = qobject_cast<DeviceLister*>(sender());

  qDebug() << "Device added:" << id;

  // Do we have this device already?
  int i = FindDeviceById(id);
  if (i != -1) {
    DeviceInfo& info = devices_[i];
    for (int backend_index = 0 ; backend_index < info.backends_.count() ; ++backend_index) {
      if (info.backends_[backend_index].unique_id_ == id) {
        info.backends_[backend_index].lister_ = lister;
        break;
      }
    }

    emit dataChanged(index(i, 0), index(i, 0));
  } else {
    // Check if we have another device with the same URL
    i = FindDeviceByUrl(lister->MakeDeviceUrls(id));
    if (i != -1) {
      // Add this device's lister to the existing device
      DeviceInfo& info = devices_[i];
      info.backends_ << DeviceInfo::Backend(lister, id);

      // If the user hasn't saved the device in the DB yet then overwrite the
      // device's name and icon etc.
      if (info.database_id_ == -1 && info.BestBackend()->lister_ == lister) {
        info.friendly_name_ = lister->MakeFriendlyName(id);
        info.size_ = lister->DeviceCapacity(id);
        info.LoadIcon(lister->DeviceIcons(id), info.friendly_name_);
      }

      emit dataChanged(index(i, 0), index(i, 0));
    } else {
      // It's a completely new device
      DeviceInfo info;
      info.backends_ << DeviceInfo::Backend(lister, id);
      info.friendly_name_ = lister->MakeFriendlyName(id);
      info.size_ = lister->DeviceCapacity(id);
      info.LoadIcon(lister->DeviceIcons(id), info.friendly_name_);

      beginInsertRows(QModelIndex(), devices_.count(), devices_.count());
      devices_ << info;
      endInsertRows();
    }
  }
}

void DeviceManager::PhysicalDeviceRemoved(const QString &id) {
  DeviceLister* lister = qobject_cast<DeviceLister*>(sender());

  qDebug() << "Device removed:" << id;

  int i = FindDeviceById(id);
  if (i == -1) {
    // Shouldn't happen
    return;
  }

  DeviceInfo& info = devices_[i];

  if (info.database_id_ != -1) {
    // Keep the structure around, but just "disconnect" it
    for (int backend_index = 0 ; backend_index < info.backends_.count() ; ++backend_index) {
      if (info.backends_[backend_index].unique_id_ == id) {
        info.backends_[backend_index].lister_ = NULL;
        break;
      }
    }

    if (info.device_ && info.device_->lister() == lister)
      info.device_.reset();

    emit dataChanged(index(i, 0), index(i, 0));

    if (!info.device_)
      emit DeviceDisconnected(i);
  } else {
    // If this was the last lister for the device then remove it from the model
    for (int backend_index = 0 ; backend_index < info.backends_.count() ; ++backend_index) {
      if (info.backends_[backend_index].unique_id_ == id) {
        info.backends_.removeAt(backend_index);
        break;
      }
    }

    if (info.backends_.isEmpty()) {
      beginRemoveRows(QModelIndex(), i, i);
      devices_.removeAt(i);

      foreach (const QModelIndex& idx, persistentIndexList()) {
        if (idx.row() == i)
          changePersistentIndex(idx, QModelIndex());
        else if (idx.row() > i)
          changePersistentIndex(idx, index(idx.row()-1, idx.column()));
      }

      endRemoveRows();
    }
  }
}

void DeviceManager::PhysicalDeviceChanged(const QString &id) {
  DeviceLister* lister = qobject_cast<DeviceLister*>(sender());
  Q_UNUSED(lister);

  int i = FindDeviceById(id);
  if (i == -1) {
    // Shouldn't happen
    return;
  }

  // TODO
}

boost::shared_ptr<ConnectedDevice> DeviceManager::Connect(int row) {
  DeviceInfo& info = devices_[row];
  if (info.device_) // Already connected
    return info.device_;

  boost::shared_ptr<ConnectedDevice> ret;

  if (!info.BestBackend()->lister_) // Not physically connected
    return ret;

  bool first_time = (info.database_id_ == -1);
  if (first_time) {
    // We haven't stored this device in the database before
    info.database_id_ = backend_->AddDevice(info.SaveToDb());
  }

  // Get the device URLs
  QList<QUrl> urls = info.BestBackend()->lister_->MakeDeviceUrls(
      info.BestBackend()->unique_id_);
  if (urls.isEmpty())
    return ret;

  // Take the first URL that we have a handler for
  QUrl device_url;
  foreach (const QUrl& url, urls) {
    qDebug() << "Connecting" << url;

    // Find a device class for this URL's scheme
    if (device_classes_.contains(url.scheme())) {
      device_url = url;
    }
  }

  if (device_url.isEmpty()) {
    // Munge the URL list into a string list
    QStringList url_strings;
    foreach (const QUrl& url, urls) { url_strings << url.toString(); }

    emit Error(tr("This type of device is not supported: %1").arg(url_strings.join(", ")));
    return ret;
  }

  QMetaObject meta_object = device_classes_.value(device_url.scheme());
  QObject* instance = meta_object.newInstance(
      Q_ARG(QUrl, device_url), Q_ARG(DeviceLister*, info.BestBackend()->lister_),
      Q_ARG(QString, info.BestBackend()->unique_id_), Q_ARG(DeviceManager*, this),
      Q_ARG(int, info.database_id_), Q_ARG(bool, first_time));
  ret.reset(static_cast<ConnectedDevice*>(instance));

  if (!ret) {
    qWarning() << "Could not create device for" << device_url.toString();
  } else {
    info.device_ = ret;
    emit dataChanged(index(row), index(row));
    connect(info.device_.get(), SIGNAL(TaskStarted(int)), SLOT(DeviceTaskStarted(int)));
    connect(info.device_.get(), SIGNAL(Error(QString)), SIGNAL(Error(QString)));
  }

  emit DeviceConnected(row);

  return ret;
}

boost::shared_ptr<ConnectedDevice> DeviceManager::GetConnectedDevice(int row) const {
  return devices_[row].device_;
}

int DeviceManager::GetDatabaseId(int row) const {
  return devices_[row].database_id_;
}

DeviceLister* DeviceManager::GetLister(int row) const {
  return devices_[row].BestBackend()->lister_;
}

void DeviceManager::Disconnect(int row) {
  DeviceInfo& info = devices_[row];
  if (!info.device_) // Already disconnected
    return;

  info.device_.reset();
  emit DeviceDisconnected(row);
  emit dataChanged(index(row), index(row));
}

void DeviceManager::Forget(int row) {
  DeviceInfo& info = devices_[row];
  if (info.database_id_ == -1)
    return;

  if (info.device_)
    Disconnect(row);

  backend_->RemoveDevice(info.database_id_);
  info.database_id_ = -1;

  if (!info.BestBackend()->lister_) {
    // It's not attached any more so remove it from the list
    beginRemoveRows(QModelIndex(), row, row);
    devices_.removeAt(row);

    foreach (const QModelIndex& idx, persistentIndexList()) {
      if (idx.row() == row)
        changePersistentIndex(idx, QModelIndex());
      else if (idx.row() > row)
        changePersistentIndex(idx, index(idx.row()-1, idx.column()));
    }

    endRemoveRows();
  } else {
    // It's still attached, set the name and icon back to what they were
    // originally
    const QString id = info.BestBackend()->unique_id_;

    info.friendly_name_ = info.BestBackend()->lister_->MakeFriendlyName(id);
    info.LoadIcon(info.BestBackend()->lister_->DeviceIcons(id), info.friendly_name_);

    dataChanged(index(row, 0), index(row, 0));
  }
}

void DeviceManager::SetDeviceIdentity(int row, const QString &friendly_name,
                                      const QString &icon_name) {
  DeviceInfo& info = devices_[row];
  info.friendly_name_ = friendly_name;
  info.LoadIcon(QStringList() << icon_name, friendly_name);

  emit dataChanged(index(row, 0), index(row, 0));

  if (info.database_id_ != -1)
    backend_->SetDeviceIdentity(info.database_id_, friendly_name, icon_name);
}

void DeviceManager::DeviceTaskStarted(int id) {
  ConnectedDevice* device = qobject_cast<ConnectedDevice*>(sender());
  if (!device)
    return;

  for (int i=0 ; i<devices_.count() ; ++i) {
    DeviceInfo& info = devices_[i];
    if (info.device_.get() == device) {
      active_tasks_[id] = index(i);
      info.task_percentage_ = 0;
      emit dataChanged(index(i), index(i));
      return;
    }
  }
}

void DeviceManager::TasksChanged() {
  QList<TaskManager::Task> tasks = task_manager_->GetTasks();
  QList<QPersistentModelIndex> finished_tasks = active_tasks_.values();

  foreach (const TaskManager::Task& task, tasks) {
    if (!active_tasks_.contains(task.id))
      continue;

    QPersistentModelIndex index = active_tasks_[task.id];
    if (!index.isValid())
      continue;

    DeviceInfo& info = devices_[index.row()];
    if (task.progress_max)
      info.task_percentage_ = float(task.progress) / task.progress_max * 100;
    else
      info.task_percentage_ = 0;
    emit dataChanged(index, index);
    finished_tasks.removeAll(index);
  }

  foreach (const QPersistentModelIndex& index, finished_tasks) {
    if (!index.isValid())
      continue;

    DeviceInfo& info = devices_[index.row()];
    info.task_percentage_ = -1;
    emit dataChanged(index, index);
  }
}

void DeviceManager::UnmountAsync(int row) {
  Q_ASSERT(QMetaObject::invokeMethod(this, "Unmount", Q_ARG(int, row)));
}

void DeviceManager::Unmount(int row) {
  DeviceInfo& info = devices_[row];
  if (info.database_id_ == -1)
    return;

  if (info.device_)
    Disconnect(row);

  if (info.BestBackend()->lister_)
    info.BestBackend()->lister_->UnmountDevice(info.BestBackend()->unique_id_);
}
