// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NETMESSAGEMAKER_H
#define BITCOIN_NETMESSAGEMAKER_H

#include <net.h>
#include <serialize.h>

class CNetMsgMaker
{
public:
    explicit CNetMsgMaker(int nVersionIn) : nVersion(nVersionIn){}

    template <typename... Args>
    CSerializedNetMsg Make(int nFlags, std::string msg_type, Args&&... args) const
    {
        CSerializedNetMsg msg;
        msg.m_type = std::move(msg_type);
        // Blackcoin ToDo: revert after nodes upgrade to current version
        int32_t serModes = nVersion <= OLD_VERSION ? SER_NETWORK : SER_NETWORK | SER_POSMARKER;
        VectorWriter{serModes, msg.data, 0, std::forward<Args>(args)...};
        return msg;
    }

    template <typename... Args>
    CSerializedNetMsg Make(std::string msg_type, Args&&... args) const
    {
        return Make(0, std::move(msg_type), std::forward<Args>(args)...);
    }

private:
    const int nVersion;
};

#endif // BITCOIN_NETMESSAGEMAKER_H
