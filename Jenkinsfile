GIT_COMMIT_EMAIL = ""
GIT_COMMIT_HASH = ""
REGISTRY_URL = "registry.ci.leil.io"

def getLeilTestsRef() {
    def ref = params?.LEIL_TESTS_REF?.trim()
    if (!ref) {
        return 'refs/tags/v0.8.0'
    }

    if (ref.startsWith('refs/')) {
        return ref
    }

    if (ref ==~ /^v\d+\.\d+\.\d+([.-].*)?$/) {
        return "refs/tags/${ref}"
    }

    return ref
}

def buildLeilTests() {
    dir('leil-tests') {
        deleteDir()
        checkout([
            $class: 'GitSCM',
            branches: [[name: getLeilTestsRef()]],
            doGenerateSubmoduleConfigurations: false,
            userRemoteConfigs: [[
                url: 'https://github.com/leil-io/leil-tests.git'
            ]]
        ])
    }
    sh '''
        cd leil-tests
        go build -o $WORKSPACE/leil-tests
        '''
}

def buildImage(imageName) {
    sh """
        cd $WORKSPACE
        mkdir -p build
        docker buildx build --build-arg BASE_IMAGE=${imageName} --tag leil-test:latest -f tests/docker/Dockerfile.test $WORKSPACE
        """
}

def pushImage(registryImageName) {
    sh """
        docker tag leil-test:latest ${registryImageName}
        docker push ${registryImageName}
        """
}

def runSanity() {
    def resultsFile = "test_results_sanity.xml"
    sh """
        ./leil-tests/leil-tests \
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
        ./leil-tests/leil-tests \
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
    sh """ ./leil-tests/leil-tests \
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
    sh """ ./leil-tests/leil-tests \
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

if(env.BRANCH_NAME?.equals("dev")) {
    properties([disableConcurrentBuilds()])
} else {
    properties([disableConcurrentBuilds(abortPrevious: true)])
}

pipeline {
    agent none

    parameters {
        booleanParam(
            name: 'DEPLOY_PACKAGE_TO_EXPERIMENTAL',
            defaultValue: false,
            description: 'Build and deploy packages to EXPERIMENTAL'
        )
        string(
            name: 'DEBIAN_PACKAGE_REF',
            defaultValue: 'dev',
            description: 'If deploying packages, what leil-io/debian-package reference to use?'
        )
        string(
            name: 'LEIL_TESTS_REF',
            defaultValue: 'v0.8.0',
            description: 'leil-tests ref: branch (dev, fix/foo), tag (v0.8.0), refs/*, or SHA'
        )
    }

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
                    GIT_COMMIT_HASH = sh(
                        script: 'git rev-parse HEAD',
                        returnStdout: true,
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
                stage('Run special tests (Ubuntu 24.04)') {
                    agent {label 'unittests'}
                    stages {
                        stage("Checkout source") {
                            steps {
                                checkout scm
                            }
                        }
                        stage("Build and unit test") {
                            steps {
                                sh """
                                    export PATH="/usr/lib/ccache:$PATH"
                                    cd vcpkg
                                    ./bootstrap-vcpkg.sh
                                    cd ..
                                    mkdir -p build
                                    cd build
                                    nice cmake \
                                         -DCMAKE_TOOLCHAIN_FILE="../vcpkg/scripts/buildsystems/vcpkg.cmake" \
                                         -DENABLE_CLIENT_LIB=ON \
                                         -DENABLE_DOCS=ON \
                                         -DENABLE_NFS_GANESHA=ON \
                                         -DENABLE_URAFT=ON \
                                         -DGSH_CAN_HOST_LOCAL_FS=ON \
                                         -DCMAKE_INSTALL_PREFIX="/usr/local/" \
                                         -DCMAKE_BUILD_TYPE=RelWithDebInfo \
                                         -DENABLE_TESTS=ON \
                                         -DCODE_COVERAGE=OFF \
                                         -DSAUNAFS_TEST_POINTER_OBFUSCATION=ON \
                                         -DENABLE_WERROR=ON \
                                         -DENABLE_FOUNDATIONDB=ON \
                                    ..
                                    cd src/unittests
                                    nice make -j\$((\$(nproc) / 2))
                                    # Make sure that failures do NOT fail the pipeline
                                    sudo systemctl restart foundationdb
                                    ./unittests || true
                                """
                                publishJunit("build/src/unittests/*test_detail.xml")
                            }
                        }
                        stage("Build and run rebalancing tests") {
                            steps {
                                sh """
                                    export PATH="/usr/lib/ccache:$PATH"
                                    cd build
                                    sudo nice make -j\$((\$(nproc) / 2)) install
                                    leil-tests --gtest_filter="RebalancingTests*" --gtest_output="xml:./rebalance_test_detail.xml" || true
                                """
                                publishJunit("build/*test_detail.xml")
                            }
                        }
                    }
                    post {
                        failure {
                            slackBadMessage(
                                "Unit tests failed on ${BRANCH_NAME}",
                                "Unit tests failed on branch ${BRANCH_NAME}, build number ${BUILD_NUMBER}"
                            )
                        }
                        cleanup {
                            sh """
                                sudo chown \$(id -u):\$(id -g) $WORKSPACE -R
                            """
                        }
                    }
                }
                stage('Build with clang') {
                    agent {label 'build'}
                    steps {
                        checkout scm
                        script {
                            sh """
                                docker buildx build --tag leilfs-clang-build:latest -f tests/docker/Dockerfile.test $WORKSPACE
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
                                docker image rm leilfs-clang-build || true
                                """
                        }
                    }
                }
                stage('Build Ubuntu 22.04') {
                    environment {
                        SANITY_WORKERS = "${env.SANITY_WORKERS ?: '4'}"
                        SHORT_WORKERS = "${env.SHORT_WORKERS ?: '4'}"
                        LONG_WORKERS = "${env.LONG_WORKERS ?: '4'}"
                        MACHINE_MULTIPLIER = "${env.MACHINE_MULTIPLIER ?: '5'}"
                        REGISTRY_IMAGE_NAME = "${REGISTRY_URL}/ubuntu22.04-leil-test:$GIT_COMMIT"
                    }
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
                                    pushImage(env.REGISTRY_IMAGE_NAME)
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
                            sh """
                                docker image rm ${env.REGISTRY_IMAGE_NAME} || true
                                docker image rm leil-test:latest || true
                                """
                        }
                    }
                }
                stage('Build Ubuntu 24.04') {
                    agent { label "test && ubuntu-2404" }
                    environment {
                        SANITY_WORKERS = "${env.SANITY_WORKERS ?: '4'}"
                        SHORT_WORKERS = "${env.SHORT_WORKERS ?: '4'}"

                        MACHINE_MULTIPLIER = "${env.MACHINE_MULTIPLIER ?: '5'}"
                        REGISTRY_IMAGE_NAME = "${REGISTRY_URL}/ubuntu24.04-leil-test:$GIT_COMMIT"
                    }
                    stages {
                        stage("Checkout source") {
                            steps {
                                checkout scm
                            }
                        }
                        stage('Build leil-tests') {
                            steps {
                                buildLeilTests()
                            }
                        }


                        stage('Build image') {
                            steps {
                                script {
                                    buildImage("ubuntu:24.04")
                                    pushImage(env.REGISTRY_IMAGE_NAME)
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
                        // Clean after build
                        cleanup {
                            cleanWs(cleanWhenNotBuilt: true,
                                deleteDirs: true,
                                disableDeferredWipeout: true,
                                notFailBuild: true,
                            )
                            sh '''
                                docker rm $(docker stop $(docker ps -a -q --filter ancestor=leil-test --format="{{.ID}}")) || true
                                '''
                            sh """
                                docker image rm ${env.REGISTRY_IMAGE_NAME} || true
                                docker image rm leil-test:latest || true
                                """
                        }
                    }
                }
                stage('Long system tests (Ubuntu 24.04)') {
                    when {
                        beforeAgent true
                        anyOf {
                            branch 'dev'
                            branch 'gh-readonly-queue/*'
                            branch 'stable'
                            changeRequest()
                        }
                    }
                    agent { label "test && ubuntu-2404" }
                    environment {
                        LONG_WORKERS = "${env.LONG_WORKERS ?: '4'}"
                        MACHINE_MULTIPLIER = "${env.MACHINE_MULTIPLIER ?: '5'}"
                    }
                    stages {
                        stage("Checkout source") {
                            steps {
                                checkout scm
                            }
                        }
                        stage('Build leil-tests') {
                            steps {
                                buildLeilTests()
                            }
                        }
                        stage('Build image') {
                            steps {
                                script {
                                    buildImage("ubuntu:24.04")
                                }
                            }
                        }
                        stage('Run long system tests') {
                            steps {
                                runLong()
                            }
                        }
                    }
                    post {
                        unstable {
                            script {
                                slackBadMessage(
                                    "Long tests failed to pass on ${BRANCH_NAME}",
                                    "Long tests failed on branch ${BRANCH_NAME}, build number ${BUILD_NUMBER}"
                                )
                            }
                        }
                        failure {
                            script {
                                slackBadMessage(
                                    "Long tests pipeline failed on ${BRANCH_NAME}",
                                    "Long tests pipeline failed on branch ${BRANCH_NAME}, build number ${BUILD_NUMBER}"
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
                                docker rm $(docker stop $(docker ps -a -q --filter ancestor=leil-test --format="{{.ID}}")) || true
                                '''
                            sh """
                                docker image rm leil-test:latest || true
                                """
                        }
                    }
                }
            }
        }
        stage('Deploy packages to dev repository') {
            agent none
            when {
                anyOf {
                    branch "dev"
                    expression { return params.DEPLOY_PACKAGE_TO_EXPERIMENTAL }
                }
            }
            steps {
                script {
                    def repo = params.DEPLOY_PACKAGE_TO_EXPERIMENTAL
                                ? "Experimental"
                                : "Development"
                    build (
                        job: 'SaunaFS Packages (Dev)',
                        parameters: [
                            string(name: 'PACKAGE_REF', value: params.DEBIAN_PACKAGE_REF),
                            string(name: 'SAUNAFS_REF', value: GIT_COMMIT_HASH),
                            string(name: 'REPOSITORY', value: repo)
                        ]
                    )
                }
            }
        }
    }
    post {
        // Runs only when every parallel branch is done, including the long
        // tests, unlike the previous per-branch success notification.
        success {
            script {
                slackGoodMessage(
                    "Pipeline passed on branch ${BRANCH_NAME}, build number ${BUILD_NUMBER}"
                )
            }
        }
    }
}
