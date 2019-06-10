//TODO LIST
// - optimize evil node so that do transactions with node far

#include "App.h"

using namespace omnetpp;

const int INITIAL_MONEY = 100;
const int EVIL_NODE_ID[] = { 1 }; // [-1 means that there are no evil node]
const int EVIL_SLEEPING_TRANSACTION = 10; // [1 is MIN]
const int EVIL_NUMBER_OF_TRANSACTION = 2; // [2 is MIN] total max number of transaction to perform after the first evil transaction

Define_Module(App);

App::App()
{
    timerThread = nullptr;
}

App::~App()
{
    cancelAndDelete(timerThread);
}

void App::initialize()
{
    myAddress = par("address");
    packetLengthBytes = &par("packetLength");
    sendIATime = &par("sendIaTime");
    sendIaTimeEvil = &par("sendIaTimeEvil");
    pkCounter = 0;

    tempBlockID = -1;
    tempPartnerSeqNum = 0;
    tempBlockTransaction = 0;

    //Information To Log
    WATCH(pkCounter);
    WATCH(myAddress);
    WATCH(chainTotalValue);

    //Obsolete
//    const char *destAddressesPar = par("destAddresses");
//    cStringTokenizer tokenizer(destAddressesPar);
//    const char *token;
//    while ((token = tokenizer.nextToken()) != nullptr)
//        destAddresses.push_back(atoi(token));

//Neighbors definition
    const int totalNodes = par("totalNodes");
    for (int i = 0; i < totalNodes; i++) {
        destAddresses.push_back(i);
    }

    if (destAddresses.size() == 0)
        throw cRuntimeError("At least one address must be specified in the destAddresses parameter!");

    //TrustChain initialization
    registerNewChainNode(-1, -1, INITIAL_MONEY);

    //Node status definition
    amIEvil = false;
    int i = 0;
    for (i = 0; i < sizeof(EVIL_NODE_ID) / sizeof(EVIL_NODE_ID[0]); i++) {
        if (myAddress == EVIL_NODE_ID[i]) {
            amIEvil = true;
        }
    }
    totalEvilTransactions = 0;

    //Start the recursive thread
    timerThread = new cMessage("TimerThead");
    scheduleAt(sendIATime->doubleValue(), timerThread);

    endToEndDelaySignal = registerSignal("endToEndDelay");
    hopCountSignal = registerSignal("hopCount");
    sourceAddressSignal = registerSignal("sourceAddress");
}

void App::handleMessage(cMessage *msg)
{
    if (msg == timerThread) {
        threadFunction();
    }
    else {
        receiveMessage(msg);
    }
}

void App::threadFunction()
{
    if (tempBlockID == -1) { //No transaction are pending
        calculateChainValue();
        if (chainTotalValue > 0) {
            if (isNodeEvil() && totalEvilTransactions >= EVIL_NUMBER_OF_TRANSACTION) {
                return;
            }
            else {
                createTransactionMessage();
            }
        }
    }

    if (isNodeEvil()) {
        sendIATime = sendIaTimeEvil;
    }
    scheduleAt(simTime() + sendIATime->doubleValue(), timerThread);

//    if (!isNodeEvil()) {
//        scheduleAt(simTime() + sendIATime->doubleValue(), timerThread);
//    }
//    else {
//        if (totalEvilTransactions + 1 < EVIL_NUMBER_OF_TRANSACTION) {
//            scheduleAt(simTime() + sendIaTimeEvil->doubleValue(), timerThread);
//        }
//        else {
//            char text[128];
//            sprintf(text, "Evil node #%d is now performing his last transaction Time: %s s", myAddress, SIMTIME_STR(simTime()));
//            EV << text << endl;
//            getSimulation()->getActiveEnvir()->alert(text);
//            // what if that is busy?
//        }
//    }
}

void App::receiveMessage(cMessage *msg)
{
    if (hasGUI())
        getParentModule()->bubble("Arrived!");
    Packet *pk = check_and_cast<Packet *>(msg);

    emit(endToEndDelaySignal, simTime() - pk->getCreationTime());
    emit(hopCountSignal, pk->getHopCount());
    emit(sourceAddressSignal, pk->getSrcAddr());

    switch (pk->getPacketType()) {
        case 0: { // Transaction Request Received

            if (tempBlockID != -1) { //currently in another transaction
                createBusyMessage(pk->getSrcAddr());
                break;
            }

            calculateChainValue();
            if (isNodeEvil() && chainTotalValue != 0) { //ignore all the incoming transactions
                createBusyMessage(pk->getSrcAddr());
                break;
            }

            tempBlockID = pk->getSrcAddr();
            tempPartnerSeqNum = pk->getMyChainSeqNum();
            tempBlockTransaction = pk->getTransactionValue();
            createChainRequestMessage();
            break;
        }
        case 1: { // Chain Request Received
            createChainLogMessage();
            break;
        }
        case 2: { // Partner Chain Received

            if (verificationTransactionChain(pk)) {
                logTransactionChain(pk);

                registerNewChainNode(tempBlockID, tempPartnerSeqNum, tempBlockTransaction);
                createAckMessage();
                if (!isNodeEvil()) {
                    createDisseminationMessage(myAddress, trustChain.size(), tempBlockID, tempPartnerSeqNum, tempBlockTransaction);
                }
            }
            else {
                char text[128];
                sprintf(text, "Node: #%d - detected double spending in verification done by #%d distant: %d Time: %s s", myAddress, tempBlockID, pk->getHopCount(), SIMTIME_STR(simTime()));
                EV << text << endl;
                getSimulation()->getActiveEnvir()->alert(text);
                stopSimulation(tempBlockID);
            }
            tempBlockID = -1;
            tempBlockTransaction = 0;
            break;
        }
        case 3: { // Ack Received
            if (pk->getSrcAddr() == tempBlockID) {
                if (!isNodeEvil()) {
                    registerNewChainNode(tempBlockID, pk->getMyChainSeqNum(), -tempBlockTransaction);
                    createDisseminationMessage(myAddress, trustChain.size(), tempBlockID, pk->getMyChainSeqNum(), -tempBlockTransaction);
                }
                else {
                    char text[128];
                    sprintf(text, "Evil node: #%d - completed transaction with #%d of value $%d distant: %d Time: %s s", myAddress, tempBlockID, tempBlockTransaction, pk->getHopCount(), SIMTIME_STR(simTime()));
                    EV << text << endl;
                    getSimulation()->getActiveEnvir()->alert(text);
                    totalEvilTransactions = totalEvilTransactions + 1;
                    victimDestAddresses.push_back(tempBlockID);
                }

                tempBlockID = -1;
                tempBlockTransaction = 0;
            }
            break;
        }
        case 4: { // Dissemination Received
            int result = verificationDissemination(pk);
            if (result == -1) {
                LogDatabaseElement *element = new LogDatabaseElement(pk->getUserXID(), pk->getUserXSeqNum(), pk->getUserYID(), pk->getUserYSeqNum(), pk->getTransactionValue());
                if (!isAlreadyPresentInDb(element)) {
                    logDatabase.push_back(*element);
                }
            }
            else {
                char text[128];
                sprintf(text, "Node: #%d - detected double spending in dissemination done by #%d Time: %s s", myAddress, result, SIMTIME_STR(simTime()));
                EV << text << endl;
                getSimulation()->getActiveEnvir()->alert(text);
                stopSimulation(result);
            }
            break;
        }
        case 5: { // Busy Received
            if (pk->getSrcAddr() == tempBlockID) {
                tempBlockID = -1;
                tempBlockTransaction = 0;
            }
            break;
        }
        default: {
            break;
        }
    }
    delete pk;

}

void App::createBusyMessage(int destAddress)
{

    char pkname[40];
    sprintf(pkname, "#%ld from-%d-to-%d busy", pkCounter++, myAddress, destAddress);

    if (hasGUI())
        getParentModule()->bubble("Generating packet...");

    //Packet Creation
    Packet *pk = new Packet(pkname);
    pk->setByteLength(packetLengthBytes->intValue());
    pk->setSrcAddr(myAddress);
    pk->setDestAddr(destAddress);

    pk->setPacketType(5);

    send(pk, "out");
}

void App::createTransactionMessage()
{
    //Calculate transaction value
    int transactionValue = intuniform(1, chainTotalValue);

    //Decide destination
    int destAddress = randomNodeAddressPicker();
    if (isNodeEvil()) {
        while (itIsAlreadyBeenAttacked(destAddress)) {
            destAddress = randomNodeAddressPicker();
        }
        simulationRegisterTransactionTime(myAddress);
    }

    tempBlockID = destAddress;
    tempBlockTransaction = transactionValue;

    if (isNodeEvil()) {
        char text[128];
        sprintf(text, "Evil node: #%d - starting transaction with #%d of value $%d Time: %s s", myAddress, tempBlockID, tempBlockTransaction, SIMTIME_STR(simTime()));
        EV << text << endl;
        getSimulation()->getActiveEnvir()->alert(text);
    }

    char pkname[40];
    sprintf(pkname, "#%ld from-%d-to-%d $%d", pkCounter++, myAddress, destAddress, transactionValue);

    if (hasGUI())
        getParentModule()->bubble("Generating packet...");

    //Packet Creation
    Packet *pk = new Packet(pkname);
    pk->setByteLength(packetLengthBytes->intValue());
    pk->setSrcAddr(myAddress);
    pk->setDestAddr(destAddress);

    pk->setPacketType(0);
    pk->setTransactionValue(transactionValue);
    pk->setMyChainSeqNum(trustChain.size() + 1);

    send(pk, "out");
}

void App::createChainRequestMessage()
{
    int destAddress = tempBlockID;

    char pkname[40];
    sprintf(pkname, "#%ld from-%d-to-%d chain request", pkCounter++, myAddress, destAddress);

    if (hasGUI())
        getParentModule()->bubble("Generating packet...");

    //Packet Creation
    Packet *pk = new Packet(pkname);
    pk->setByteLength(packetLengthBytes->intValue());
    pk->setSrcAddr(myAddress);
    pk->setDestAddr(destAddress);

    pk->setPacketType(1);

    send(pk, "out");
}

void App::createChainLogMessage()
{
    int destAddress = tempBlockID;

    char pkname[40];
    sprintf(pkname, "#%ld from-%d-to-%d chain log", pkCounter++, myAddress, destAddress);

    if (hasGUI())
        getParentModule()->bubble("Generating packet...");

    //Packet Creation
    Packet *pk = new Packet(pkname);
    pk->setByteLength(packetLengthBytes->intValue() + ((trustChain.size() - 1) * 10));
    pk->setSrcAddr(myAddress);
    pk->setDestAddr(destAddress);

    pk->setPacketType(2);

    //Add chain to the block

    if (trustChain.size() != 0) {
        pk->setUserBIDArraySize(trustChain.size());
        pk->setUserBSeqNumArraySize(trustChain.size());
        pk->setTransactionArraySize(trustChain.size());
        int i;
        for (i = 0; i < trustChain.size(); i++) {
            pk->setUserBID(i, trustChain[i].partnerId);
            pk->setUserBSeqNum(i, trustChain[i].partnerSeqNum);
            pk->setTransaction(i, trustChain[i].transactionValue);
        }
    }
    else {
        pk->setUserBIDArraySize(0);
        pk->setUserBSeqNumArraySize(0);
        pk->setTransactionArraySize(0);
    }

    send(pk, "out");
}

void App::createAckMessage()
{
    int destAddress = tempBlockID;

    char pkname[40];
    sprintf(pkname, "#%ld from-%d-to-%d transaction ack", pkCounter++, myAddress, destAddress);

    if (hasGUI())
        getParentModule()->bubble("Generating packet...");

    //Packet Creation
    Packet *pk = new Packet(pkname);
    pk->setByteLength(packetLengthBytes->intValue());
    pk->setSrcAddr(myAddress);
    pk->setDestAddr(destAddress);

    pk->setPacketType(3);
    pk->setMyChainSeqNum(trustChain.size());

    send(pk, "out");
}

bool App::verificationTransactionChain(Packet *pk)
{
    //check if the your local information can be compared to the received informations
    // 1 - same sequence number and user ids number have the same transaction value (chain block replaced with another one)
    // 2 - All the local info regarding a user can be found in the chain (not hiding something)

    int i;
    for (i = 0; i < logDatabase.size(); i++) {

        if (logDatabase[i].UserAId == pk->getSrcAddr()) {
            int index = logDatabase[i].UserASeqNum - 1;
            if (pk->getUserBIDArraySize() > index) {
                if (pk->getUserBID(index) == logDatabase[i].UserBId && pk->getUserBSeqNum(index) == logDatabase[i].UserBSeqNum && pk->getTransaction(index) == logDatabase[i].transactionValue) {
                    //all good!
                }
                else {
                    //EV << "****The local transaction is not matching with the data provided" << endl;
                    return false;
                }
            }
            else {
                //EV << "****There aren't enough transactions in the received chain" << endl;
                return false;
            }
        }
        else if (logDatabase[i].UserBId == pk->getSrcAddr()) {
            int index = logDatabase[i].UserBSeqNum - 1;
            if (pk->getUserBIDArraySize() > index) {
                if (pk->getUserBID(index) == logDatabase[i].UserAId && pk->getUserBSeqNum(index) == logDatabase[i].UserASeqNum && pk->getTransaction(index) == -logDatabase[i].transactionValue) {
                    //all good!
                }
                else {
                    //EV << "****The local transaction is not matching with the data provided B" << endl;
                    return false;
                }
            }
            else {
                //EV << "****There aren't enough transactions in the received chain B" << endl;
                return false;
            }

        }

    }

    return true;
}

int App::verificationDissemination(Packet *pk)
{
    int i;
    for (i = 0; i < logDatabase.size(); i++) {

        if (logDatabase[i].UserAId == pk->getUserXID() && logDatabase[i].UserASeqNum == pk->getUserXSeqNum()) {
            if (logDatabase[i].UserBId == pk->getUserYID() && logDatabase[i].UserBSeqNum == pk->getUserYSeqNum() && abs(logDatabase[i].transactionValue) == abs(pk->getTransactionValue())) {
                return -1;
            }
            else {
                return logDatabase[i].UserAId;
            }
        }

        if (logDatabase[i].UserAId == pk->getUserYID() && logDatabase[i].UserASeqNum == pk->getUserYSeqNum()) {
            if (logDatabase[i].UserBId == pk->getUserXID() && logDatabase[i].UserBSeqNum == pk->getUserXSeqNum() && abs(logDatabase[i].transactionValue) == abs(pk->getTransactionValue())) {
                return -1;
            }
            else {
                return logDatabase[i].UserAId;
            }
        }

        if (logDatabase[i].UserBId == pk->getUserXID() && logDatabase[i].UserBSeqNum == pk->getUserXSeqNum()) {
            if (logDatabase[i].UserAId == pk->getUserYID() && logDatabase[i].UserASeqNum == pk->getUserYSeqNum() && abs(logDatabase[i].transactionValue) == abs(pk->getTransactionValue())) {
                return -1;
            }
            else {
                return logDatabase[i].UserBId;
            }
        }

        if (logDatabase[i].UserBId == pk->getUserYID() && logDatabase[i].UserBSeqNum == pk->getUserYSeqNum()) {
            if (logDatabase[i].UserAId == pk->getUserXID() && logDatabase[i].UserASeqNum == pk->getUserXSeqNum() && abs(logDatabase[i].transactionValue) == abs(pk->getTransactionValue())) {
                return -1;
            }
            else {
                return logDatabase[i].UserBId;
            }
        }

    }

    return -1;
}

void App::logTransactionChain(Packet *pk)
{
    if (pk->getUserBIDArraySize() != 0) {
        int i;
        for (i = 1; i < pk->getUserBIDArraySize(); i++) {
            LogDatabaseElement *element = new LogDatabaseElement(pk->getSrcAddr(), i + 1, pk->getUserBID(i), pk->getUserBSeqNum(i), pk->getTransaction(i));
            if (!isAlreadyPresentInDb(element)) {
                logDatabase.push_back(*element);
                //EV << " **************new element" << endl;
            }
            else {
                //EV << " ***************already present " << endl;
            }
        }
    }
}

bool App::isAlreadyPresentInDb(LogDatabaseElement *element)
{
    int i;
    for (i = 0; i < logDatabase.size(); i++) {
        if (logDatabase[i].UserAId == element->UserAId && logDatabase[i].UserASeqNum == element->UserASeqNum && logDatabase[i].UserBId == element->UserBId && logDatabase[i].UserBSeqNum == element->UserBSeqNum) {
            return true;
        }
        if (logDatabase[i].UserAId == element->UserBId && logDatabase[i].UserASeqNum == element->UserBSeqNum && logDatabase[i].UserBId == element->UserAId && logDatabase[i].UserBSeqNum == element->UserASeqNum) {
            return true;
        }
    }
    return false;
}

void App::createDisseminationMessage(int userXID, int userXSeqNum, int userYID, int userYSeqNum, int transactionValue)
{

    int i;
    for (i = 0; i < destAddresses.size(); i++) {

// Sending packet
        int destAddress = destAddresses[i];
        if (destAddress == myAddress) {
            continue;
        }

        char pkname[40];
        sprintf(pkname, "#%ld from-%d-to-%d dissemination", pkCounter++, myAddress, destAddress);

        if (hasGUI())
            getParentModule()->bubble("Generating packet...");

//Packet Creation
        Packet *pk = new Packet(pkname);
        pk->setByteLength(packetLengthBytes->intValue());
        pk->setSrcAddr(myAddress);
        pk->setDestAddr(destAddress);

        pk->setPacketType(4);

        pk->setUserXID(userXID);
        pk->setUserXSeqNum(userXSeqNum);
        pk->setUserYID(userYID);
        pk->setUserYSeqNum(userYSeqNum);
        pk->setTransactionValue(transactionValue);

        send(pk, "out");

    }
}

int App::randomNodeAddressPicker()
{
    int destAddress = -1;

    while (destAddress == -1 || destAddress == myAddress) {
        destAddress = destAddresses[intuniform(0, destAddresses.size() - 1)];
    }

    return destAddress;
}

void App::registerNewChainNode(int id, int seqNum, int value)
{
    TrustChainElement *chainElement = new TrustChainElement(id, seqNum, value);
    trustChain.push_back(*chainElement);
    calculateChainValue();

    if (id != -1) {
        LogDatabaseElement *element = new LogDatabaseElement(myAddress, trustChain.size(), id, seqNum, value);
        logDatabase.push_back(*element);
    }
}

void App::calculateChainValue()
{
    int localTotalChainValue = 0, i = 0;

    for (i = 0; i < trustChain.size(); i++) {
        localTotalChainValue = localTotalChainValue + trustChain[i].transactionValue;
    }
    chainTotalValue = localTotalChainValue;
}

bool App::isNodeEvil()
{
    if (amIEvil && trustChain.size() >= EVIL_SLEEPING_TRANSACTION) {
        return true;
    }
    else {
        return false;
    }
}

bool App::itIsAlreadyBeenAttacked(int nodeId)    // The evil node can check here if a node as been already addressed or not
{
    int i;
    for (i = 0; i < victimDestAddresses.size(); i++) {
        if (victimDestAddresses[i] == nodeId) {
            return true;
        }
    }
    return false;
}

void App::simulationRegisterTransactionTime(int nodeId) // Here are recorded the transaction starting times for all evil nodes
{
    int i = 0, alreadyRegistered = 0;

    for (i = 0; i < simulationTiming.size(); i++) {
        if (simulationTiming[i].nodeId == nodeId) {
            alreadyRegistered = 1;
            simulationTiming[i].transactionTime = simTime();
        }
    }

    if (alreadyRegistered == 0) {
        SimulationTiming *timeLog = new SimulationTiming(nodeId, simTime(), 0);
        simulationTiming.push_back(*timeLog);
    }
}

void App::simulationRegisterDetectionTime(int nodeId) // Here are logged the evil nodes detection times
{
    int i = 0;

    for (i = 0; i < simulationTiming.size(); i++) {
        if (simulationTiming[i].nodeId == nodeId) {
            if (simulationTiming[i].detectionTime == 0) { // In case of multiple detection caused by dissemination_detection we just need the quickest time
                simulationTiming[i].detectionTime = simTime();
            }
        }
    }
}

void App::stopSimulation(int evilNodeId) // This function is being called when ever an evil node is being detected
{
    simulationRegisterDetectionTime(evilNodeId);

    int i, nodesDetected = 0;
    for (i = 0; i < simulationTiming.size(); i++) {
        if (simulationTiming[i].detectionTime != 0) {
            nodesDetected++;
        }
    }

    // If there has been detected enough nodes equal to the evil node numbers stop simulation
    if (nodesDetected == sizeof(EVIL_NODE_ID) / sizeof(EVIL_NODE_ID[0])) {

        char text[128];
        for (i = 0; i < simulationTiming.size(); i++) {
            sprintf(text, "Simulation: evil node #%d delta detection time: %s s ", simulationTiming[i].nodeId, SIMTIME_STR(simulationTiming[i].detectionTime - simulationTiming[i].transactionTime));
            getSimulation()->getActiveEnvir()->alert(text);
        }

        simulationTiming.clear();
        endSimulation();
    }
}

//DEBUG CODE
//            if (myAddress == 0) {
//
//                int i;
//                EV << "logDatabase:" << endl;
//                for (i = 0; i < logDatabase.size(); i++) {
//                    EV << "User A: " << logDatabase[i].UserAId << " User A Seq N: " << logDatabase[i].UserASeqNum << " User B: " << logDatabase[i].UserBId << " User B Seq N: " << logDatabase[i].UserBSeqNum
//                            << " Transac: " << logDatabase[i].transactionValue << endl;
//                }
//
//                char text[128];
//                sprintf(text, "I am  #%d and i received dissemination #%d #%d <> #%d #%d $#%d ", myAddress, pk->getUserXID(), pk->getUserXSeqNum(), pk->getUserYID(), pk->getUserYSeqNum(), pk->getTransactionValue());
//                getSimulation()->getActiveEnvir()->alert(text);
//
//            }
