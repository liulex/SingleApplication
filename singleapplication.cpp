// The MIT License (MIT)
//
// Copyright (c) Itay Grudev 2015 - 2018
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include <QtCore/QTime>
#include <QtCore/QThread>
#include <QtCore/QDateTime>
#include <QtCore/QByteArray>
#include <QtCore/QSharedMemory>

#include "singleapplication.h"
#include "singleapplication_p.h"

/**
 * @brief Constructor. Checks and fires up LocalServer or closes the program
 * if another instance already exists
 * @param argc
 * @param argv
 * @param {bool} allowSecondaryInstances
 */
SingleApplication::SingleApplication( int &argc, char *argv[], bool allowSecondary, Options options, const QByteArray &extraHashData, int timeout )
    : app_t( argc, argv ), d_ptr( new SingleApplicationPrivate( this ) )
{
    Q_D(SingleApplication);

    // Store the current mode of the program
    d->options = options;

    // Generating an application ID used for identifying the shared memory
    // block and QLocalServer
    d->genBlockServerName( extraHashData );

#ifdef Q_OS_UNIX
    // By explicitly attaching it and then deleting it we make sure that the
    // memory is deleted even after the process has crashed on Unix.
    d->memory = new QSharedMemory( d->blockServerName );
    d->memory->attach();
    delete d->memory;
#endif
    // Guarantee thread safe behaviour with a shared memory block.
    d->memory = new QSharedMemory( d->blockServerName );

    // Create a shared memory block
    if( d->memory->create( sizeof( InstancesInfo ) ) ) {
        // Initialize the shared memory block
        d->memory->lock();
        d->initializeMemoryBlock();
        d->memory->unlock();
    } else {
        // Attempt to attach to the memory segment
        if( ! d->memory->attach() ) {
            qCritical() << "SingleApplication: Unable to attach to shared memory block.";
            qCritical() << d->memory->errorString();
            delete d;
            ::exit( EXIT_FAILURE );
        }
    }

    InstancesInfo* inst = static_cast<InstancesInfo*>( d->memory->data() );
    QTime time;
    time.start();

    // Make sure the shared memory block is initialised and in consistent state
    while( true ) {
        d->memory->lock();

        if( d->blockChecksum() == inst->checksum ) break;

        if( time.elapsed() > 5000 ) {
            qWarning() << "SingleApplication: Shared memory block has been in an inconsistent state from more than 5s. Assuming primary instance failure.";
            d->initializeMemoryBlock();
        }

        d->memory->unlock();

        // Random sleep here limits the probability of a collision between two racing apps
        qsrand( QDateTime::currentMSecsSinceEpoch() % std::numeric_limits<uint>::max() );
        QThread::sleep( 8 + static_cast <unsigned long>( static_cast <float>( qrand() ) / RAND_MAX * 10 ) );
    }

    if( inst->primary == false) {
        d->startPrimary();
        d->memory->unlock();
        return;
    }

    // Check if another instance can be started
    if( allowSecondary ) {
        inst->secondary += 1;
        inst->checksum = d->blockChecksum();
        d->instanceNumber = inst->secondary;
        d->startSecondary();
        if( d->options & Mode::SecondaryNotification ) {
            d->connectToPrimary( timeout, SingleApplicationPrivate::SecondaryInstance );
        }
        d->memory->unlock();
        return;
    }

    d->memory->unlock();

    d->connectToPrimary( timeout, SingleApplicationPrivate::NewInstance );

    delete d;

    ::exit( EXIT_SUCCESS );
}

/**
 * @brief Destructor
 */
SingleApplication::~SingleApplication()
{
    Q_D(SingleApplication);
    delete d;
}

bool SingleApplication::isPrimary()
{
    Q_D(SingleApplication);
    return d->server != nullptr;
}

bool SingleApplication::isSecondary()
{
    Q_D(SingleApplication);
    return d->server == nullptr;
}

quint32 SingleApplication::instanceId()
{
    Q_D(SingleApplication);
    return d->instanceNumber;
}

qint64 SingleApplication::primaryPid()
{
    Q_D(SingleApplication);
    return d->primaryPid();
}

bool SingleApplication::sendMessage( QByteArray message, int timeout )
{
    Q_D(SingleApplication);

    // Nobody to connect to
    if( isPrimary() ) return false;

    // Make sure the socket is connected
    d->connectToPrimary( timeout,  SingleApplicationPrivate::Reconnect );

    d->socket->write( message );
    bool dataWritten = d->socket->waitForBytesWritten( timeout );
    d->socket->flush();
    return dataWritten;
}
