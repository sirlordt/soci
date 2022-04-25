//
// Copyright (C) 2004-2008 Maciej Sobczak
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef SOCI_TRANSACTION_H_INCLUDED
#define SOCI_TRANSACTION_H_INCLUDED

#include "soci/soci-platform.h"
#include "soci/session.h"

namespace soci
{

enum transaction_status {
    disabled   = 0,
    active     = 1,
    commited   = 2,
    rolledback = 3
};

class SOCI_DECL transaction
{
public:
    explicit transaction(session& sql);

    ~transaction();

    void commit();
    void rollback();

    session & current_session() const;

    bool is_active() const;
    bool by_session() const;

    transaction_status status() const;

private:
    //Private constructor use from session object in case not assigned transaction.
    //Used in src/core/session.cpp:343
    transaction(session& sql, bool by_session);

    bool handled_;
    session & sql_;
    bool by_session_;
    // 0 = Disabled
    // 1 = Active
    // 2 = Commited
    // 3 = Rolled back
    transaction_status status_;

    void disabled();
    void active();
    void commited();
    void rolledback();

    friend class session;

    SOCI_NOT_COPYABLE(transaction)
};

} // namespace soci

#endif // SOCI_TRANSACTION_H_INCLUDED
