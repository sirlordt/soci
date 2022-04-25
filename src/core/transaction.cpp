//
// Copyright (C) 2004-2008 Maciej Sobczak
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//

#define SOCI_SOURCE
#include "soci/transaction.h"
#include "soci/error.h"

using namespace soci;

transaction::transaction(session& sql)
    : handled_(false), sql_(sql), by_session_(false), status_(transaction_status::active) //Active
{
    if (sql_.current_transaction() == NULL ||  //Session already in transaction?
        sql_.allow_multiple_transaction())     //Session allow multiple transactions?
    {
        if (sql_.current_transaction() &&
            sql_.current_transaction()->by_session()) //The session already with transaction?
        {
            transaction * currentTransaction = sql_.current_transaction();

            //Yes is created by session object.
            bool is_active = currentTransaction->is_active();

            currentTransaction->disabled();
            // current_transaction->handled_ = true; //Disable the transaction
            // current_transaction->status_  = 0;    //disabled

            if (is_active) //Only if needed call to rollback
            {
                sql_.rollback();           //Revert in session transaction.
            }
        }

        sql_.externalTransaction_ = this; //Pass the reference back. This transaction is created outside of session object
        sql_.begin_external_transaction();
    }
    else
    {
        status_ = transaction_status::disabled; //Disabled
        handled_ = true; //Yes. Session already in transaction and not allow mutiple transactions. Auto disable this transaction object
    }
}

//Private constructor use from session object in case not assigned transaction.
//Used in src/core/session.cpp:343
transaction::transaction(session& sql, bool by_session)
    : handled_(false), sql_(sql), by_session_(by_session), status_(transaction_status::active) //Active
{
    //This instance is created from inside of session object.
}

transaction::~transaction()
{
    if (is_active())
    {
        try
        {
            rollback();
            this->rolledback();
        }
        catch (...)
        {}
    }

    if (sql_.externalTransaction_ == this)
    {
        sql_.externalTransaction_ = NULL; //Clear the reference in the session object
    }
}

void transaction::commit()
{
    if (handled_ || this->status_ != transaction_status::active)
    {
        throw soci_error("The transaction object cannot be handled twice.");
    }

    sql_.commit();
    this->commited();
    // status_ = 2; //Commited
    // handled_ = true;
}

void transaction::rollback()
{
    if (handled_ || this->status_ != transaction_status::active)
    {
        throw soci_error("The transaction object cannot be handled twice.");
    }

    sql_.rollback();
    this->rolledback();
    // status_ = 3; //Rolled back
    // handled_ = true;
}

session & transaction::current_session() const
{
    return sql_;
}

bool transaction::is_active() const
{
    return this->handled_ == false && this->status_ == transaction_status::active;
}

bool transaction::by_session() const //This transaction is auto created inside of the object session? src/core/session.cpp:291
{
    return this->by_session_;
}

// 0 = Disabled
// 1 = Active
// 2 = Commited
// 3 = Rolled back
transaction_status transaction::status() const
{
    return this->status_;
}

void transaction::disabled()
{
    this->handled_ = true;
    this->status_ = transaction_status::disabled;
}

void transaction::active()
{
    this->handled_ = false;
    this->status_ = transaction_status::active;
}

void transaction::commited()
{
    this->handled_ = true;
    this->status_ = transaction_status::commited;
}

void transaction::rolledback()
{
    this->handled_ = true;
    this->status_ = transaction_status::rolledback;
}
