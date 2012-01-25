#ifndef TESTMANAGER_H
#define TESTMANAGER_H

#include <QObject>
#include "DNSSECTest.h"

#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <validator/resolver.h>
#include <validator/validator.h>

#include <QtCore/QStringList>
#include <QtCore/QVariantList>
#include <QtNetwork/QNetworkAccessManager>

#define ENABLE_RESULTS_SUBMISSION 1

#ifndef RESULTS_SUBMIT_URL
#define RESULTS_SUBMIT_URL "http://www.hardakers.net/trend-tracker/trend-tracker.cgi?type=submit&"
#endif

static const QString resultServerBaseURL = RESULTS_SUBMIT_URL;

class TestManager : public QObject
{
    Q_OBJECT
    Q_ENUMS(testType)
public:

    enum testType
        { basic_dns,
          basic_tcp,
          do_bit,
          ad_bit,
          do_has_rrsigs,
          small_edns0,
          can_get_nsec,
          can_get_nsec3,
          can_get_dnskey,
          can_get_ds
        };
    explicit TestManager(QObject *parent = 0);

    Q_INVOKABLE DNSSECTest *makeTest(testType type, QString address, QString name);
    Q_INVOKABLE QStringList loadResolvConf();

    Q_INVOKABLE void submitResults(QVariantList tests);
    Q_INVOKABLE void saveSetting(QString key, QVariant value);
    Q_INVOKABLE QVariant getSetting(QString key);
    Q_INVOKABLE QString sha1hex(QString input);

    Q_PROPERTY(QString submissionMessage  READ submissionMessage                   NOTIFY submissionMessageChanged)
    Q_PROPERTY(QString lastResultMessage  READ lastResultMessage                   NOTIFY lastResultMessageChanged)

    QString submissionMessage();
    QString lastResultMessage();

signals:
    void submissionMessageChanged();
    void aResultMessageChanged(QString message);
    void lastResultMessageChanged();

public slots:
    void responseReceived(QNetworkReply *response);
    void handleResultMessageChanged(QString message);

private:
    QObject *m_parent;
    QStringList  m_serverAddresses;
    QNetworkAccessManager *m_manager;
    QString m_submissionMessage;
    QString m_lastResultMessage;
};

#endif // TESTMANAGER_H
