GIT_COMMIT_EMAIL = ""

def buildSfstests() {
    sh '''
        git clone "https://github.com/leil-io/sfstests"
        cd sfstests
        git checkout v0.5.0
        go build -o $WORKSPACE/sfstests
        '''
}

def buildImage(imageName) {
    sh """
        cd $WORKSPACE
        mkdir build
        docker buildx build --build-arg BASE_IMAGE=${imageName} --tag saunafs-test:latest -f tests/docker/Dockerfile.test $WORKSPACE
        docker tag saunafs-test:latest registry.ci.leil.io/saunafs-test:$GIT_COMMIT
        docker push registry.ci.leil.io/saunafs-test:$GIT_COMMIT
        """
}

def runSanity() {
    def resultsFile = "test_results_sanity.xml"
    sh """
        ./sfstests/sfstests \
        --auth /etc/apt/auth.conf.d/ \
        --workers ${SANITY_WORKERS} \
        --multiplier ${MACHINE_MULTIPLIER} \
        --cpus 1 \
        --xml-path ${resultsFile}
        """
    publishJunit(resultsFile)
}
def runShort() {
    def resultsFile = "test_results_short.xml"
    sh """
        ./sfstests/sfstests \
        --auth /etc/apt/auth.conf.d/ \
        --suite ShortSystemTests \
        --workers ${SHORT_WORKERS} \
        --multiplier ${MACHINE_MULTIPLIER} \
        --cpus 2 \
        --xml-path ${resultsFile}
        """
    publishJunit(resultsFile)
}
def runMachine() {
    def resultsFile = "test_results_machine.xml"
    sh """ ./sfstests/sfstests \
        --auth /etc/apt/auth.conf.d/ \
        --suite SingleMachineTests \
        --workers 1 \
        --skip-on-fail \
        --multiplier ${MACHINE_MULTIPLIER} \
        --xml-path ${resultsFile}
        """
    publishJunit(resultsFile)
}
def runLong() {
    def resultsFile = "test_results_long.xml"
    sh """ ./sfstests/sfstests \
        --auth /etc/apt/auth.conf.d/ \
        --workers ${LONG_WORKERS} \
        --suite LongSystemTests \
        --multiplier ${MACHINE_MULTIPLIER} \
        --skip-on-fail \
        --cpus 2 \
        --xml-path ${resultsFile}
        """
    publishJunit(resultsFile)
}

def publishJunit(resultsFile) {
    junit(
        skipMarkingBuildUnstable: false,
        testResults: resultsFile,
    )
}

def slackBadMessage(channelMsg, userMsg) {
    if (env.BRANCH_NAME == 'dev') {
        slackSend(color: "danger", message: "<$BUILD_URL|$channelMsg>")
    } else {
        GIT_COMMIT_EMAIL = sh(
            script: 'git --no-pager show -s --format="%ae"',
            returnStdout: true
        ).trim()
        def userId = slackUserIdFromEmail(GIT_COMMIT_EMAIL)
        if (userId?.trim()) {
            slackSend(
                color: "warning",
                channel: "$userId",
                message: "<$BUILD_URL|$userMsg>"
            )
        } else {
            echo "Failed to retrieve Slack user ID for email: ${GIT_COMMIT_EMAIL}"
        }
    }
}

def slackGoodMessage(userMsg) {
    if (env.BRANCH_NAME == 'dev') {
        // No need to alert if everything works :)
       return
    }
    // We still want to alert developers that a pipeline passed
    def userId = slackUserIdFromEmail(GIT_COMMIT_EMAIL)
    if (userId?.trim()) {
        slackSend(
            color: "good",
            channel: "$userId",
            message: "<$BUILD_URL|$userMsg>"
        )
    } else {
        echo "Failed to retrieve Slack user ID for email: ${GIT_COMMIT_EMAIL}"
    }
}

pipeline {
    agent none
    options {
        skipStagesAfterUnstable()
        // TODO: Add more lenient rules for dev
        buildDiscarder(logRotator(artifactDaysToKeepStr: "7", artifactNumToKeepStr: "2"))
        parallelsAlwaysFailFast()
    }
    tools { go '1.22.2' }
    stages {
        stage('Get build metadata') {
            agent {label 'linux'}
            steps {
                checkout scm
                script {
                    GIT_COMMIT_EMAIL = sh(
                        script: 'git --no-pager show -s --format="%ae"',
                        returnStdout: true
                    ).trim()
                }
            }
            post {
                cleanup {
                    cleanWs(cleanWhenNotBuilt: true,
                        deleteDirs: true,
                        disableDeferredWipeout: true,
                        notFailBuild: true,
                    )
                }
            }
        }
        stage('Build and test') {
            parallel {
                stage('Build with clang') {
                    agent {label 'build'}
                    steps {
                        checkout scm
                        script {
                            sh """
                                docker buildx build --tag saunafs-clang-build:latest -f tests/docker/Dockerfile.test $WORKSPACE
                                """
                        }
                    }
                    post {
                        failure {
                            slackBadMessage(
                                "clang failed to build on ${BRANCH_NAME}",
                                "clang failed on branch ${BRANCH_NAME}, build number ${BUILD_NUMBER}"
                            )
                        }
                        cleanup {
                            cleanWs(cleanWhenNotBuilt: true,
                                deleteDirs: true,
                                disableDeferredWipeout: true,
                                notFailBuild: true,
                            )
                            sh """
                                docker image rm saunafs-clang-build || true
                                """
                        }
                    }
                }
                stage('Build Ubuntu 22.04') {
                    agent { label "build" }
                    stages {
                        stage("Checkout source") {
                            steps {
                                checkout scm
                            }
                        }
                        stage('Build image') {
                            steps {
                                script {
                                    buildImage("ubuntu:22.04")
                                }
                            }
                        }
                    }
                    post {
                        // Clean after build
                        failure {
                            slackBadMessage(
                                "Ubuntu 22.04 build failed on ${BRANCH_NAME}",
                                "Ubuntu 22.04 build failed on branch ${BRANCH_NAME}, build number ${BUILD_NUMBER}"
                            )
                        }
                        cleanup {
                            cleanWs(cleanWhenNotBuilt: true,
                                deleteDirs: true,
                                disableDeferredWipeout: true,
                                notFailBuild: true,
                            )
                            sh '''
                                docker image rm saunafs-test || true
                                '''
                        }
                    }
                }
                stage('Build Ubuntu 24.04') {
                    agent { label "test && ubuntu-2404" }
                    environment {
                        SANITY_WORKERS = "${env.SANITY_WORKERS ?: '4'}"
                        SHORT_WORKERS = "${env.SHORT_WORKERS ?: '4'}"
                        LONG_WORKERS = "${env.LONG_WORKERS ?: '4'}"

                        MACHINE_MULTIPLIER = "${env.MACHINE_MULTIPLIER ?: '5'}"
                    }
                    stages {
                        stage("Checkout source") {
                            steps {
                                checkout scm
                            }
                        }
                        stage('Build sfstests') {
                            steps {
                                buildSfstests()
                            }
                        }


                        stage('Build image') {
                            steps {
                                script {
                                    buildImage("ubuntu:24.04")
                                }
                            }
                        }

                        stage('Run Sanity') {
                            steps {
                                runSanity()
                            }
                        }

                        stage('Run short system tests') {
                            steps {
                                runShort()
                            }
                        }

                        stage('Run machine tests') {
                            when {
                                anyOf {
                                    branch 'dev'
                                    branch 'gh-readonly-queue/*'
                                    branch 'stable'
                                    changeRequest()
                                }
                            }
                            steps {
                                runMachine()
                            }
                        }

                        stage('Run long system tests') {
                            when {
                                anyOf {
                                    branch 'dev'
                                    branch 'gh-readonly-queue/*'
                                    branch 'stable'
                                    changeRequest()
                                }
                            }
                            steps {
                                runLong()
                            }
                        }
                    }
                    post {
                        unstable {
                            script {
                                slackBadMessage(
                                    "Tests failed to pass on ${BRANCH_NAME}",
                                    "Tests failed on branch ${BRANCH_NAME}, build number ${BUILD_NUMBER}"
                                )
                            }
                        }
                        failure {
                            script {
                                slackBadMessage(
                                    "Ubuntu 24.04 pipeline failed on ${BRANCH_NAME}",
                                    "Ubuntu 24.04 pipeline failed on branch ${BRANCH_NAME}, build number ${BUILD_NUMBER}"
                                )
                            }
                        }
                        success {
                            script {
                                slackGoodMessage(
                                    "Ubuntu 24.04 pipeline passed on branch ${BRANCH_NAME}, build number ${BUILD_NUMBER}"
                                )
                            }
                        }
                        // Clean after build
                        cleanup {
                            cleanWs(cleanWhenNotBuilt: true,
                                deleteDirs: true,
                                disableDeferredWipeout: true,
                                notFailBuild: true,
                            )
                            sh '''
                                docker rm $(docker stop $(docker ps -a -q --filter ancestor=saunafs-test --format="{{.ID}}")) || true
                                docker image rm saunafs-test || true
                                docker image rm registry.ci.leil.io/saunafs-test || true
                                '''
                        }
                    }
                }
            }
        }
        stage('Deploy packages to dev repository') {
            agent none
            when { branch "dev" }
            steps {
                build (
                    job: 'SaunaFS Packages (Dev)',
                    parameters: [
                        string(name: 'VERSION', value: ''),
                        string(name: 'REFERENCE', value: 'dev'),
                        string(name: 'REPOSITORY', value: 'Development')
                    ]
                )
            }
        }
    }
}
