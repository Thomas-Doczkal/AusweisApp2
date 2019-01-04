/*!
 * \copyright Copyright (c) 2016-2018 Governikus GmbH & Co. KG, Germany
 */


#include "StateEstablishPaceChannel.h"

#include "context/AuthContext.h"
#include "context/ChangePinContext.h"

Q_DECLARE_LOGGING_CATEGORY(statemachine)

using namespace governikus;

StateEstablishPaceChannel::StateEstablishPaceChannel(const QSharedPointer<WorkflowContext>& pContext)
	: AbstractGenericState(pContext, false)
	, mPasswordId(PacePasswordId::UNKNOWN)
{
}


void StateEstablishPaceChannel::run()
{
	auto cardConnection = getContext()->getCardConnection();
	if (!cardConnection)
	{
		qCDebug(statemachine) << "No card connection available.";
		abort();
		return;
	}

	QByteArray certificateDescription, effectiveChat;
	mPasswordId = getContext()->getEstablishPaceChannelType();
	Q_ASSERT(mPasswordId != PacePasswordId::UNKNOWN);
	getContext()->setEstablishPaceChannelType(PacePasswordId::UNKNOWN);

	if (mPasswordId == PacePasswordId::PACE_PIN ||
			(mPasswordId == PacePasswordId::PACE_CAN && getContext()->isCanAllowedMode()))
	{
		if (auto authContext = getContext().objectCast<AuthContext>())
		{
			// if PACE is performed for authentication purposes,
			// the chat and certificate description need to be send
			//
			// in other scenarios, e.g. for changing the PIN, the data
			// is not needed
			Q_ASSERT(authContext->getDidAuthenticateEac1());
			Q_ASSERT(!authContext->encodeEffectiveChat().isEmpty());
			certificateDescription = authContext->getDidAuthenticateEac1()->getCertificateDescriptionAsBinary();
			effectiveChat = authContext->encodeEffectiveChat();
		}
	}

	QString password;
	switch (mPasswordId)
	{
		case PacePasswordId::PACE_CAN:
			password = getContext()->getCan();
			break;

		case PacePasswordId::PACE_PIN:
			password = getContext()->getPin();
			break;

		case PacePasswordId::PACE_PUK:
			password = getContext()->getPuk();
			break;

		case PacePasswordId::UNKNOWN:
		case PacePasswordId::PACE_MRZ:
			password = QString();
			break;
	}

	if (password.isEmpty() && cardConnection->getReaderInfo().isBasicReader())
	{
		qCCritical(statemachine) << "We hit an invalid state! PACE password is empty for basic reader.";
		Q_ASSERT(false);

		qCDebug(statemachine) << "Reseting all PACE passwords.";
		getContext()->resetPacePasswords();

		abort();
		return;
	}

	qDebug() << "Establish connection using" << mPasswordId;
	Q_ASSERT(!password.isEmpty() || !cardConnection->getReaderInfo().isBasicReader());

	if (mPasswordId == PacePasswordId::PACE_PUK)
	{
		mConnections += cardConnection->callUnblockPinCommand(this, &StateEstablishPaceChannel::onEstablishConnectionDone, password);
	}
	else
	{
		mConnections += cardConnection->callEstablishPaceChannelCommand(this,
				&StateEstablishPaceChannel::onEstablishConnectionDone,
				mPasswordId,
				password,
				effectiveChat,
				certificateDescription);
	}
}


void StateEstablishPaceChannel::onUserCancelled()
{
	getContext()->setLastPaceResult(CardReturnCode::CANCELLATION_BY_USER);
	AbstractState::onUserCancelled();
}


void StateEstablishPaceChannel::abort()
{
	getContext()->resetLastPaceResult();
	Q_EMIT fireAbort();
}


void StateEstablishPaceChannel::handleNpaPosition(CardReturnCode pReturnCode)
{
	if (pReturnCode == CardReturnCode::CARD_NOT_FOUND || pReturnCode == CardReturnCode::RETRY_ALLOWED)
	{
		qCDebug(statemachine) << "Card vanished during PACE. Incrementing unfortunate-card-position panickiness.";
		getContext()->handleWrongNpaPosition();
		return;
	}

	qCDebug(statemachine) << "Clearing unfortunate-card-position panickiness. |" << pReturnCode;
	getContext()->setNpaPositionVerified();
	return;
}


void StateEstablishPaceChannel::onEstablishConnectionDone(QSharedPointer<BaseCardCommand> pCommand)
{
	if (mPasswordId != PacePasswordId::PACE_PUK)
	{
		auto paceCommand = pCommand.staticCast<EstablishPaceChannelCommand>();
		getContext()->setPaceOutputData(paceCommand->getPaceOutput());
	}

	CardReturnCode returnCode = pCommand->getReturnCode();
	getContext()->setLastPaceResult(returnCode);

	handleNpaPosition(returnCode);

	if (mPasswordId == PacePasswordId::PACE_PIN && returnCode == CardReturnCode::OK)
	{
		qCDebug(statemachine) << "PACE PIN succeeded. Setting expected retry counter to:" << 3;
		getContext()->setExpectedRetryCounter(3);
	}
	else if (mPasswordId == PacePasswordId::PACE_PUK && returnCode == CardReturnCode::OK)
	{
		qCDebug(statemachine) << "PACE PUK succeeded. Resetting PACE passwords and setting expected retry counter to:" << -1;
		getContext()->resetPacePasswords();
		getContext()->setExpectedRetryCounter(-1);
	}

	switch (returnCode)
	{
		case CardReturnCode::OK:
			if (mPasswordId == PacePasswordId::PACE_PIN ||
					(mPasswordId == PacePasswordId::PACE_CAN && getContext()->isCanAllowedMode()))
			{
				Q_EMIT firePaceChannelEstablished();
				return;
			}
			else if (mPasswordId == PacePasswordId::PACE_PUK)
			{
				getContext()->setLastPaceResult(CardReturnCode::OK_PUK);

				if (auto changePinContext = getContext().objectCast<ChangePinContext>())
				{
					changePinContext->setSuccessMessage(tr("PIN successfully unblocked"));
				}

				Q_EMIT firePacePukEstablished();
				return;
			}

			Q_EMIT fireContinue();
			return;

		case CardReturnCode::PUK_INOPERATIVE:
		case CardReturnCode::CANCELLATION_BY_USER:
			updateStatus(CardReturnCodeUtil::toGlobalStatus(returnCode));
			Q_EMIT fireAbort();
			return;

		case CardReturnCode::INVALID_PIN:
		{
			CardReturnCode paceResult;
			switch (getContext()->getCardConnection()->getReaderInfo().getRetryCounter())
			{
				case 2:
					// Old retryCounter is 2: 2nd try failed
					paceResult = CardReturnCode::INVALID_PIN_2;
					break;

				case 1:
					// Old retryCounter is 1: 3rd try failed
					paceResult = CardReturnCode::INVALID_PIN_3;
					break;

				default:
					paceResult = CardReturnCode::INVALID_PIN;
			}
			getContext()->setLastPaceResult(paceResult);
			Q_EMIT fireAbort();
			return;
		}

		default:
			if (getContext()->isNpaRepositioningRequired())
			{
				Q_EMIT fireAbortAndUnfortunateCardPosition();
				return;
			}

			Q_EMIT fireAbort();
			return;
	}

	Q_UNREACHABLE();
}