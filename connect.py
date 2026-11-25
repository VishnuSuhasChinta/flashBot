#shouldve focused in CN, socket programming man

#create a TCP socket
#connect to 10.10.100.259:8899
#print that its connected
import socket
import sys

#Documentation at https://github-wiki-see.page/m/Parallel-7/flashforge-api-docs/wiki/Adventurer-3-Series
#SESSION CONTROL
ControlSession = "~M601 S1\r\n"
ReleaseSession = "~M602\r\n"

#INFORMATION
GetInfo = "~M115\r\n"
GetPrintStatus = "~M27\r\n"
GetTemps = "~M105\r\n"
GetEndStop = "~M119\r\n"
GetCurrentPosition = "~M114\r\n"

#JOB CONTROL
#SelectAndPrintFile = "~M23 0:/user/<file>\r\n" - make this a function
ResumePrint = "~M24\r\n"
PausePrint = "~M25\r\n"
CancelPrint = "~M26\r\n"

#MOTION
HomeAllAxes = "~G28\r\n"


#FUNCTION TO SEND A COMMAND
def sendRecievePrint(sockObj, sendMessage):
    try:
        sockObj.sendall(sendMessage.encode())
        sockObj.settimeout(3) #set timeout in SECONDS, 
        message = sockObj.recv(1024) #add timeout
        temp = message

        #MESSAGE IS BYTES NOT STRING
        while("\r\n" not in message.decode()):#add some sort of timeout here too?
            if(temp == b""):
                raise Exception("Connection closed unexpectedly")
            temp = sockObj.recv(1024)
            #if closed, error
            message+=temp

        print(message.decode() + '\n')
    except Exception as e:
        print(e)
        sys.exit()

flashy = socket.socket(socket.AF_INET, socket.SOCK_STREAM ) #removed SOCK_NONBLOCK

#TRY CONNECTING TO PRINTER
try:
    flashy.connect(('10.10.100.254',8899))
    print("Successful connection\n")

except OSError as e: 
    print(e)
    sys.exit()

#create successful conneciton to start sending gcode
#send ~M601 S1

sendRecievePrint(flashy, ControlSession)
command = input("Enter Command: ")

#GET COMMANDS AND EXECUTE
while(command.upper() != "EXIT"):
    if command == "Get Info":
        sendRecievePrint(flashy, GetInfo)
    elif command == "Get Status":
        sendRecievePrint(flashy, GetPrintStatus)
    elif command == "Get Temperatures":
            sendRecievePrint(flashy, GetTemps)
    elif command == "Get Endstop":
            sendRecievePrint(flashy, GetEndStop)
    elif command == "Get Position":
            sendRecievePrint(flashy, GetCurrentPosition)
    else:
        print("Please enter a known command\n")

    command = input("Enter Command: ")
sendRecievePrint(flashy, ReleaseSession)

#close socket
flashy.close()
